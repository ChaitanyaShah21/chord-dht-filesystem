#ifndef LOG_H
#define LOG_H

#include <initializer_list>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// Structured logging (decision D-086) and request ids (D-087).
//
// Every event is ONE line of key=value fields, written with ONE write(2):
//
//   ts=2026-10-09T13:14:16.123Z level=info proc=node:7010 req=6f1c2a9e0b3d4c5a event=chunk.get key=ab12... ms=0.8
//
// A value that is empty or contains a space, '=', '"', '\' or a control character
// is written in double quotes, with '"' and '\' escaped and control characters as
// \n, \r, \t or \xNN -- so no value can end the line early or forge a field.
//
// The request id is the one that came with the request this thread is handling
// (RequestScope), or else the process's own (a client sets one per command). It
// goes into every line logged, and into every request line sent (with_request_id).
// ---------------------------------------------------------------------------

enum class Level { Debug, Info, Warn, Error };

// "debug", "info", "warn" or "error".
const char *level_name(Level l);
bool parse_level(const std::string &s, Level &out);

// Call once, at start-up, before any other thread exists: the process's name as
// it appears in every line, the lowest level written, and where lines go (a file
// descriptor; standard error by default; -1 for nowhere). Read-only afterwards.
void log_setup(const std::string &process, Level threshold, int fd = 2);

// Whether a line at this level would be written -- so a caller can skip building
// expensive fields for a debug line nobody will see.
bool log_enabled(Level l);

using Field = std::pair<const char *, std::string>;

// Write one event, if `level` is at or above the threshold. Never throws.
void log_event(Level level, const char *event, std::initializer_list<Field> fields = {});

// The line log_event writes, without its trailing newline: pure, for the tests.
std::string format_log_line(const std::string &timestamp, Level level, const std::string &process,
                            const std::string &request_id, const char *event, std::initializer_list<Field> fields);

// A value as written in a line: bare if it is safe, quoted and escaped otherwise.
std::string logfmt_value(const std::string &v);

// The longest line written, newline included: one write of at most PIPE_BUF bytes
// to a pipe is never split by another writer's. Longer lines are cut, and end in "...".
constexpr std::size_t MAX_LOG_LINE = 4096;

// ---- request ids (D-087) ----------------------------------------------------

// 16 lowercase hexadecimal characters.
bool valid_request_id(const std::string &id);

// A fresh random id, from std::random_device.
std::string new_request_id();

// The process's own id: set once, before any other thread starts (the client).
void set_process_request_id(const std::string &id);

// The id of the request this thread is handling, or the process's, or "".
std::string current_request_id();

// While one of these exists, this thread's current request id is `id` ("" for
// none); its destructor puts back the one before. RAII: the id is restored on
// every way out of a scope, a thrown exception included.
//
// thread_local storage belongs to ONE thread: a std::thread started inside the
// scope begins with no id, so work handed to another thread must carry the id
// with it and open its own scope there.
class RequestScope {
public:
    explicit RequestScope(const std::string &id);
    ~RequestScope();
    RequestScope(const RequestScope &) = delete;
    RequestScope &operator=(const RequestScope &) = delete;

private:
    std::string saved_;
};

// "RID <current id> " + line, or `line` unchanged if there is no current id.
std::string with_request_id(const std::string &line);

#endif
