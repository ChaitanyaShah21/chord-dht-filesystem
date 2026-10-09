#include "raft_log.h"

#include "lookup.h"     // split_single_spaces
#include "raft_wire.h"  // parse_u64
#include "sha1.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

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

// The directory a path is in: "a/b/raft.log" -> "a/b", "raft.log" -> ".".
std::string parent_of(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Creating the file adds a name to its directory, and flushing the file does not
// make that name durable; the directory is flushed too.
bool fsync_dir(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool read_file(const std::string &path, std::string &text, std::string &err) {
    const int in = ::open(path.c_str(), O_RDONLY);
    if (in < 0) {
        if (errno == ENOENT) return true;                    // no file yet: an empty state
        err = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
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
    return true;
}

std::string vote_word(int voted_for) { return voted_for == NOBODY ? "-" : std::to_string(voted_for); }

// Apply one record's body to `d`, refusing anything a correct writer never wrote:
// a term going backwards, a second vote in one term, an entry out of sequence or
// newer than the term, a cut beyond the end.
bool replay_one(const std::string &body, Durable &d) {
    const std::vector<std::string> w = split_single_spaces(body);
    if (w.empty()) return false;
    for (const std::string &word : w)
        if (word.empty()) return false;                      // a doubled, leading or trailing space
    if (w[0] == "TERM" && w.size() == 3) {
        std::uint64_t t = 0, v = 0;
        int vote = NOBODY;
        if (!parse_u64(w[1], t)) return false;
        if (w[2] != "-") {
            if (!parse_u64(w[2], v) || v > 1000) return false;
            vote = static_cast<int>(v);
        }
        if (t < d.term) return false;
        if (t == d.term && d.voted_for != NOBODY && d.voted_for != vote) return false;
        d.term = t;
        d.voted_for = vote;
        return true;
    }
    if (w[0] == "ENTRY" && w.size() >= 4) {
        std::uint64_t i = 0, t = 0;
        if (!parse_u64(w[1], i) || !parse_u64(w[2], t)) return false;
        if (i != d.log.size() + 1 || t > d.term) return false;
        if (!d.log.empty() && t < d.log.back().term) return false;
        // The command is the rest of the body, after "ENTRY <index> <term> ".
        const std::size_t at = w[0].size() + w[1].size() + w[2].size() + 3;
        d.log.push_back(Entry{t, body.substr(at)});
        return true;
    }
    if (w[0] == "CUT" && w.size() == 2) {
        std::uint64_t i = 0;
        if (!parse_u64(w[1], i) || i == 0 || i > d.log.size()) return false;
        d.log.resize(i - 1);
        return true;
    }
    return false;
}

}  // namespace

std::string log_record(const std::string &body) { return body + " " + check_of(body); }

bool parse_log_record(const std::string &line, std::string &body) {
    const std::size_t space = line.rfind(' ');
    if (space == std::string::npos || line.size() - space - 1 != 8) return false;
    body = line.substr(0, space);
    return line.compare(space + 1, 8, check_of(body)) == 0;
}

FileStorage::~FileStorage() {
    if (fd_ >= 0) close(fd_);
}

bool FileStorage::open(const std::string &path, Durable &out, std::string &report, std::string &err) {
    std::string text;
    if (!read_file(path, text, err)) return false;

    // Replay every record up to the first bad one. `good` is the byte offset just
    // past the last good record.
    Durable d;
    std::size_t pos = 0, good = 0, records = 0;
    bool damaged = false;
    std::string why;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { damaged = true; why = "a torn final record"; break; }
        std::string body;
        const std::string line = text.substr(pos, nl - pos);
        if (!parse_log_record(line, body)) { damaged = true; why = "a record that fails its check"; break; }
        if (!replay_one(body, d)) { damaged = true; why = "a record out of sequence"; break; }
        pos = good = nl + 1;
        ++records;
    }

    // What follows the damage, for the report: intact-looking records there mean the
    // damage was not simply the end of the file.
    std::size_t after = 0;
    if (damaged) {
        std::size_t p = text.find('\n', good);
        while (p != std::string::npos && p + 1 < text.size()) {
            const std::size_t nl = text.find('\n', p + 1);
            if (nl == std::string::npos) break;
            std::string body;
            if (parse_log_record(text.substr(p + 1, nl - p - 1), body)) ++after;
            p = nl;
        }
        if (text.size() - good > LOG_TAIL_LIMIT) {
            err = path + ": " + why + " at byte " + std::to_string(good) + ", " + std::to_string(text.size() - good) +
                  " bytes from the end -- further back than a crash can damage (" + std::to_string(LOG_TAIL_LIMIT) +
                  " bytes); refusing to start rather than forget a vote or an acknowledged entry";
            return false;
        }
    }

    fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd_ < 0) {
        err = "cannot open " + path + " for appending: " + std::strerror(errno);
        return false;
    }
    // Cut a crash's tail off, so the next batch starts on a line of its own.
    if (damaged && (ftruncate(fd_, static_cast<off_t>(good)) != 0 || fsync(fd_) != 0)) {
        err = "cannot cut the damaged tail off " + path + ": " + std::strerror(errno);
        return false;
    }
    const bool dir_synced = fsync_dir(parent_of(path));

    term_ = d.term;
    voted_for_ = d.voted_for;
    last_ = d.log.size();
    out = std::move(d);
    report = std::to_string(records) + " records replayed from " + path + ": term " + std::to_string(term_) + ", " +
             std::to_string(last_) + " entries";
    if (damaged)
        report += "; " + why + " and the " + std::to_string(text.size() - good) + " bytes after it cut off" +
                  (after ? " (" + std::to_string(after) + " intact-looking records among them)" : "");
    if (!dir_synced) report += " (warning: the log's directory could not be flushed)";
    return true;
}

bool FileStorage::save(Term term, int voted_for, Index from, const std::vector<Entry> &log) {
    if (fd_ < 0) return false;
    std::string batch;
    if (term != term_ || voted_for != voted_for_)
        batch += log_record("TERM " + std::to_string(term) + " " + vote_word(voted_for)) + "\n";
    if (from <= last_) batch += log_record("CUT " + std::to_string(from)) + "\n";
    for (Index i = from; i <= log.size(); ++i)
        batch += log_record("ENTRY " + std::to_string(i) + " " + std::to_string(log[i - 1].term) + " " + log[i - 1].command) +
                 "\n";
    if (batch.empty()) return true;

    // One write and one flush for the whole step. If either fails, part of the batch
    // may be on disk; the replica stops (the core treats false as fatal), and the next
    // start cuts the partial batch off as a crash's tail.
    if (!write_all(fd_, batch.data(), batch.size()) || fdatasync(fd_) != 0) return false;
    term_ = term;
    voted_for_ = voted_for;
    last_ = log.size();
    ++batches_;
    return true;
}
