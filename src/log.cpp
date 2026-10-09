#include "log.h"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace {

// Set once by log_setup before any other thread starts; only read afterwards.
std::string g_process = "?";
Level g_threshold = Level::Info;
int g_fd = 2;
std::string g_process_request_id;

// The request this thread is handling. `thread_local`: every thread has its own
// copy, created empty when the thread starts, so connection threads handling
// different clients' requests never see each other's ids.
thread_local std::string t_request_id;

// Now, in UTC, to the millisecond: 2026-10-09T13:14:16.123Z.
std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);                    // the _r form: no shared static buffer, safe across threads
    char buf[96];                            // room for any int the format could print
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

bool needs_quotes(const std::string &v) {
    if (v.empty()) return true;
    for (unsigned char c : v)
        if (c <= 0x20 || c == '=' || c == '"' || c == '\\' || c == 0x7f) return true;
    return false;
}

}  // namespace

const char *level_name(Level l) {
    switch (l) {
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

bool parse_level(const std::string &s, Level &out) {
    for (Level l : {Level::Debug, Level::Info, Level::Warn, Level::Error})
        if (s == level_name(l)) { out = l; return true; }
    return false;
}

void log_setup(const std::string &process, Level threshold, int fd) {
    g_process = process;
    g_threshold = threshold;
    g_fd = fd;
}

bool log_enabled(Level l) { return g_fd >= 0 && static_cast<int>(l) >= static_cast<int>(g_threshold); }

std::string logfmt_value(const std::string &v) {
    if (!needs_quotes(v)) return v;
    std::string out = "\"";
    for (unsigned char c : v) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "\\x%02x", c);
            out += hex;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string format_log_line(const std::string &timestamp, Level level, const std::string &process,
                            const std::string &request_id, const char *event, std::initializer_list<Field> fields) {
    std::string line = "ts=" + timestamp + " level=" + level_name(level) + " proc=" + logfmt_value(process);
    if (!request_id.empty()) line += " req=" + logfmt_value(request_id);
    line += std::string(" event=") + logfmt_value(event);
    for (const Field &f : fields) line += std::string(" ") + f.first + "=" + logfmt_value(f.second);
    return line;
}

void log_event(Level level, const char *event, std::initializer_list<Field> fields) {
    if (!log_enabled(level)) return;
    try {
        std::string line = format_log_line(utc_now(), level, g_process, current_request_id(), event, fields);
        // Cut to fit one atomic write; a cut line says so at its end.
        if (line.size() > MAX_LOG_LINE - 1) line = line.substr(0, MAX_LOG_LINE - 4) + "...";
        line += '\n';
        // ONE write for the whole line: lines from different threads never mix
        // (D-086). The loop is for the rare short write (a full disk, a signal);
        // only then could another thread's line land between the two pieces.
        std::size_t done = 0;
        while (done < line.size()) {
            const ssize_t n = write(g_fd, line.data() + done, line.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;                         // nowhere to write: drop the line
            done += static_cast<std::size_t>(n);
        }
    } catch (...) {
        // Logging must never take a process down: out of memory here drops the line.
    }
}

// ---------------------------------------------------------------------------
// Request ids
// ---------------------------------------------------------------------------

bool valid_request_id(const std::string &id) {
    if (id.size() != 16) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string new_request_id() {
    std::random_device rd;                     // the operating system's random source
    const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

void set_process_request_id(const std::string &id) { g_process_request_id = id; }

std::string current_request_id() { return t_request_id.empty() ? g_process_request_id : t_request_id; }

RequestScope::RequestScope(const std::string &id) : saved_(t_request_id) { t_request_id = id; }

RequestScope::~RequestScope() { t_request_id = saved_; }

std::string with_request_id(const std::string &line) {
    const std::string id = current_request_id();
    return id.empty() ? line : "RID " + id + " " + line;
}
