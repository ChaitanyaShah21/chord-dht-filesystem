#include "names.h"

#include "lookup.h"     // split_single_spaces: the one strict word splitter
#include "net.h"        // MAX_LINE: the longest reply
#include "sha1.h"
#include "store.h"      // valid_key: a manifest key is a chunk key

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

// The record's self-check: 8 hexadecimal characters of SHA-1 of the record's body.
// 32 bits, so a damaged record passes by chance about once in 4 billion.
std::string check_of(const std::string &body) { return sha1(body).substr(0, 8); }

// write() may accept fewer bytes than asked -- a short write -- so loop.
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

// The directory a path is in: "a/b/names.log" -> "a/b", "names.log" -> ".".
std::string parent_of(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Creating the log file adds a name to its directory, and the file's own flush
// does not make that name durable; the directory has to be flushed as well.
bool fsync_dir(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

enum class Applied { Added, Same, Conflict };

// The state change itself, and nothing else -- D-009's split, kept. The live path
// and replay both call it, so recovery cannot take a different path through it.
// The caller holds ns.mu (or, during replay, no other thread exists yet).
Applied apply_register(Namespace &ns, const std::string &name, const std::string &key) {
    const auto it = ns.names.find(name);
    if (it == ns.names.end()) {
        ns.names.emplace(name, key);
        return Applied::Added;
    }
    return it->second == key ? Applied::Same : Applied::Conflict;
}

std::string register_name(Namespace &ns, const std::string &name, const std::string &key) {
    std::lock_guard<std::mutex> lock(ns.mu);

    // Written once (D-059): the same key again is a retry and succeeds; a
    // different key is refused, and nothing is logged either way.
    const auto it = ns.names.find(name);
    if (it != ns.names.end()) return it->second == key ? "OK" : "EXISTS " + it->second;
    if (ns.log_fd < 0 || ns.log_broken) return "ERR the name log is unavailable";

    // Write ahead: the record is on disk before the map changes and before the
    // answer, so an acknowledged name survives a crash.
    const std::string record = names_record(name, key) + "\n";
    const off_t before = lseek(ns.log_fd, 0, SEEK_END);
    if (before < 0) return "ERR could not write the name log";
    if (!write_all(ns.log_fd, record.data(), record.size()) || fdatasync(ns.log_fd) != 0) {
        // Part of the record may have been written. Cut it off: otherwise the next
        // record would continue this broken line, fail its check at replay, and an
        // acknowledged name would be lost. If even that fails, refuse every later
        // registration rather than risk it.
        if (ftruncate(ns.log_fd, before) != 0) ns.log_broken = true;
        return "ERR could not write the name log";
    }
    apply_register(ns, name, key);
    return "OK";
}

std::string resolve_name(const Namespace &ns, const std::string &name) {
    std::lock_guard<std::mutex> lock(ns.mu);
    const auto it = ns.names.find(name);
    return it == ns.names.end() ? "MISSING" : "FILE " + it->second;
}

// One page of names after `after` (from the start if `from_start`), as many as fit
// in one reply line. The count is at most 4 digits: one line holds at most 512
// one-character names.
std::string list_names(const Namespace &ns, const std::string &after, bool from_start) {
    const std::size_t budget = MAX_LINE - std::strlen("NAMES 9999");
    std::lock_guard<std::mutex> lock(ns.mu);
    auto it = from_start ? ns.names.begin() : ns.names.upper_bound(after);
    std::string page;
    int count = 0;
    for (; it != ns.names.end(); ++it) {
        if (page.size() + 1 + it->first.size() > budget) break;
        page += " " + it->first;
        ++count;
    }
    return "NAMES " + std::to_string(count) + page;
}

}  // namespace

bool valid_name(const std::string &name) {
    if (name.empty() || name.size() > MAX_NAME) return false;
    // Compared as unsigned bytes: plain char is signed on x86-64 and unsigned on
    // ARM, and a byte of 0x80 or more must be refused on both.
    for (unsigned char c : name)
        if (c < 0x21 || c > 0x7e) return false;
    return true;
}

std::string names_record(const std::string &name, const std::string &key) {
    const std::string body = "REGISTER " + name + " " + key;
    return body + " " + check_of(body);
}

bool parse_record(const std::string &line, std::string &name, std::string &key) {
    const std::vector<std::string> w = split_single_spaces(line);
    if (w.size() != 4 || w[0] != "REGISTER" || !valid_name(w[1]) || !valid_key(w[2])) return false;
    if (w[3] != check_of("REGISTER " + w[1] + " " + w[2])) return false;
    name = w[1];
    key  = w[2];
    return true;
}

bool names_open(Namespace &ns, const std::string &path, std::string &report, std::string &err) {
    ns.log_path = path;

    // Read the whole log. A missing file is an empty namespace, not an error.
    std::string text;
    const int in = open(path.c_str(), O_RDONLY);
    if (in < 0 && errno != ENOENT) {
        err = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
    if (in >= 0) {
        char buf[65536];
        while (true) {
            const ssize_t n = read(in, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                err = "cannot read " + path + ": " + std::strerror(errno);
                close(in);
                return false;
            }
            if (n == 0) break;
            text.append(buf, static_cast<std::size_t>(n));
        }
        close(in);
    }

    // Replay every complete line. Only a line that ends in '\n' is complete: the
    // newline is the last byte of each append, so a crash mid-append leaves a
    // final line without one.
    int applied = 0, skipped = 0, conflicts = 0;
    std::size_t pos = 0, complete = 0;
    bool torn = false;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { torn = true; break; }
        const std::string line = text.substr(pos, nl - pos);
        pos = complete = nl + 1;
        std::string name, key;
        if (!parse_record(line, name, key)) { ++skipped; continue; }
        if (apply_register(ns, name, key) == Applied::Conflict) ++conflicts;
        else ++applied;
    }

    const int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        err = "cannot open " + path + " for appending: " + std::strerror(errno);
        return false;
    }
    // Cut a torn tail off, so the next record starts on a line of its own.
    if (torn && (ftruncate(fd, static_cast<off_t>(complete)) != 0 || fsync(fd) != 0)) {
        err = "cannot cut the torn record off " + path + ": " + std::strerror(errno);
        close(fd);
        return false;
    }
    const bool dir_synced = fsync_dir(parent_of(path));
    ns.log_fd = fd;

    report = std::to_string(applied) + " names replayed from " + path;
    if (skipped)   report += ", " + std::to_string(skipped) + " damaged records skipped";
    if (conflicts) report += ", " + std::to_string(conflicts) + " conflicting records skipped";
    if (torn)      report += ", a torn final record (" + std::to_string(text.size() - complete) + " bytes) cut off";
    if (!dir_synced) report += " (warning: the log's directory could not be flushed)";
    return true;
}

std::string handle_names_request(Namespace &ns, const std::string &line) {
    if (line.empty()) return "ERR empty request";
    const std::vector<std::string> w = split_single_spaces(line);
    const std::string &verb = w[0];

    if (verb == "PING" && w.size() == 1) return "PONG";

    if (verb == "REGISTER") {
        if (w.size() != 3) return "ERR usage: REGISTER <name> <manifest-key>";
        if (!valid_name(w[1])) return "ERR a name is 1 to 200 visible ASCII characters";
        if (!valid_key(w[2])) return "ERR a key is 40 lowercase hexadecimal characters";
        return register_name(ns, w[1], w[2]);
    }

    if (verb == "RESOLVE") {
        if (w.size() != 2) return "ERR usage: RESOLVE <name>";
        if (!valid_name(w[1])) return "ERR a name is 1 to 200 visible ASCII characters";
        return resolve_name(ns, w[1]);
    }

    if (verb == "LIST") {
        if (w.size() > 2) return "ERR usage: LIST [<after>]";
        if (w.size() == 2 && !valid_name(w[1])) return "ERR a name is 1 to 200 visible ASCII characters";
        return list_names(ns, w.size() == 2 ? w[1] : "", w.size() == 1);
    }

    return "ERR unknown request";
}
