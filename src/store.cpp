#include "store.h"
#include "sha1.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

// The two-character fan-out directory a key lives in: "ab" for "ab12...".
std::string fan_dir(const std::string &dir, const std::string &key) {
    return dir + "/" + key.substr(0, 2);
}

// Create a directory if it does not exist. An existing directory is success.
bool make_dir(const std::string &path, std::string &err) {
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    err = "cannot create " + path + ": " + std::strerror(errno);
    return false;
}

// Flush a directory's entries to disk. A rename changes the directory, not the
// file, so the file's own fsync does not make the new name durable.
bool fsync_dir(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

// write() may accept fewer bytes than asked -- a short write -- so loop, exactly
// as send_line does for a socket.
bool write_all(int fd, const char *data, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// The names in one directory, without "." and "..". Empty if it cannot be read.
std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> names;
    DIR *d = opendir(path.c_str());
    if (!d) return names;
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    closedir(d);
    return names;
}

const char HEX[] = "0123456789abcdef";

}  // namespace

bool valid_key(const std::string &key) {
    if (key.size() != 40) return false;
    for (char c : key)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string chunk_key(const std::string &bytes) {
    return sha1_bytes(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
}

std::string store_path(const ChunkStore &s, const std::string &key) {
    return fan_dir(s.dir, key) + "/" + key;
}

const char *put_result_name(PutResult r) {
    switch (r) {
        case PutResult::Stored:       return "stored";
        case PutResult::AlreadyHeld:  return "already held";
        case PutResult::BadKey:       return "key must be 40 lowercase hexadecimal characters";
        case PutResult::TooLarge: {
            // Built from MAX_CHUNK so the chunk-size sweep build reports its own limit.
            // A function-local static is initialised once, thread-safely (C++11).
            static const std::string too_large =
                "chunk larger than " + std::to_string(MAX_CHUNK) + " bytes";
            return too_large.c_str();
        }
        case PutResult::HashMismatch: return "bytes do not hash to the key";
        case PutResult::IoError:      return "disk error";
    }
    return "disk error";
}

bool store_open(ChunkStore &s, const std::string &dir, std::string &report, std::string &err) {
    s.dir = dir;
    if (!make_dir(dir, err) || !make_dir(dir + "/tmp", err)) return false;
    for (char a : std::string(HEX))
        for (char b : std::string(HEX))
            if (!make_dir(dir + "/" + a + b, err)) return false;

    // A temporary file is a write that never reached its rename: the node died
    // between creating it and committing it. It was never reported as stored.
    int removed = 0;
    for (const std::string &name : list_dir(dir + "/tmp"))
        if (unlink((dir + "/tmp/" + name).c_str()) == 0) ++removed;

    // The index: every correctly named file in its correct fan-out directory.
    // Anything else is not ours and is left alone, but counted.
    int loaded = 0, foreign = 0;
    std::set<std::string> keys;
    for (char a : std::string(HEX)) {
        for (char b : std::string(HEX)) {
            const std::string sub = std::string(1, a) + b;
            for (const std::string &name : list_dir(dir + "/" + sub)) {
                if (valid_key(name) && name.compare(0, 2, sub) == 0) { keys.insert(name); ++loaded; }
                else ++foreign;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.keys = std::move(keys);
    }
    report = std::to_string(loaded) + " chunks, " + std::to_string(removed) +
             " unfinished writes removed, " + std::to_string(foreign) + " other files ignored";
    return true;
}

PutResult store_put(ChunkStore &s, const std::string &key, const std::string &body) {
    // Cheapest checks first, and the key before anything touches a path.
    if (!valid_key(key)) return PutResult::BadKey;
    if (body.size() > MAX_CHUNK) return PutResult::TooLarge;
    if (chunk_key(body) != key) return PutResult::HashMismatch;
    if (store_has(s, key)) return PutResult::AlreadyHeld;

    // A name no other writer in this process will choose. Two processes never
    // share a store directory, so the process number is not needed.
    const std::string tmp = s.dir + "/tmp/" + key + "." + std::to_string(s.next_tmp++);
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return PutResult::IoError;

    // Write, flush to the device, close. Only a file that got this far is ever
    // renamed into place, so a final name never holds part of a chunk.
    const bool written = write_all(fd, body.data(), body.size()) && fsync(fd) == 0;
    const bool closed  = close(fd) == 0;
    if (!written || !closed) {
        unlink(tmp.c_str());
        return PutResult::IoError;
    }

    // The commit point (D-045). rename() within one filesystem replaces the
    // destination atomically: a reader sees no file, or the whole chunk.
    if (rename(tmp.c_str(), store_path(s, key).c_str()) != 0) {
        unlink(tmp.c_str());
        return PutResult::IoError;
    }
    if (!fsync_dir(fan_dir(s.dir, key))) return PutResult::IoError;

    std::lock_guard<std::mutex> lock(s.mu);
    s.keys.insert(key);
    return PutResult::Stored;
}

GetResult store_get(const ChunkStore &s, const std::string &key, std::string &out) {
    out.clear();
    if (!valid_key(key)) return GetResult::BadKey;

    const int fd = open(store_path(s, key).c_str(), O_RDONLY);
    if (fd < 0) return errno == ENOENT ? GetResult::Missing : GetResult::IoError;

    // A file larger than any chunk is not one of ours, whatever its name: refuse
    // it rather than read an arbitrary amount into memory.
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || static_cast<std::size_t>(st.st_size) > MAX_CHUNK) {
        close(fd);
        return GetResult::IoError;
    }

    // read() may return fewer bytes than asked; loop until end of file.
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = read(fd, &out[done], out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;                       // 0: the file was shorter than fstat said
        done += static_cast<std::size_t>(n);
    }
    close(fd);
    if (done != out.size()) { out.clear(); return GetResult::IoError; }
    return GetResult::Found;
}

bool store_has(const ChunkStore &s, const std::string &key) {
    std::lock_guard<std::mutex> lock(s.mu);
    return s.keys.count(key) > 0;
}

std::vector<std::string> store_keys(const ChunkStore &s) {
    std::lock_guard<std::mutex> lock(s.mu);
    return std::vector<std::string>(s.keys.begin(), s.keys.end());
}

std::vector<std::string> store_keys_in_arc(const ChunkStore &s, Id from, Id to) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(s.mu);
    for (const std::string &key : s.keys) {
        Id id;
        if (id_from_key(key, id) && in_range_oc(id, from, to)) out.push_back(key);
    }
    return out;
}

bool store_remove(ChunkStore &s, const std::string &key) {
    if (!valid_key(key)) return false;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.keys.erase(key);
    }
    const bool ok = unlink(store_path(s, key).c_str()) == 0 || errno == ENOENT;
    fsync_dir(fan_dir(s.dir, key));
    return ok;
}
