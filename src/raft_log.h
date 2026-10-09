#ifndef RAFT_LOG_H
#define RAFT_LOG_H

#include "raft.h"

#include <cstddef>
#include <string>

// ---------------------------------------------------------------------------
// A replica's durable state as one append-only file (decision D-074):
//
//   TERM <term> <voted-for | -> <check>      the current term and the vote in it
//   ENTRY <index> <term> <command> <check>   one log entry
//   CUT <index> <check>                      entries from <index> on are discarded
//
// <check> is the first 8 hexadecimal characters of SHA-1 of everything before
// " <check>" -- the single tracker's record check (D-058). Each save() appends one
// batch and flushes it with one fdatasync before returning.
//
// Replay stops at the first record that is torn (no newline), fails its check, or
// is out of sequence. A crash can damage only the batch being written when it
// struck, and no batch is longer than LOG_TAIL_LIMIT, so damage that starts within
// that distance of the end is a crash's tail: it is cut off and reported. Damage
// further back means flushed data changed, and open() refuses: the replica would
// otherwise forget a vote or an acknowledged entry.
// ---------------------------------------------------------------------------

constexpr std::size_t LOG_TAIL_LIMIT = 256 * 1024;

// One record for `body`, without its newline.
std::string log_record(const std::string &body);

// Check one line (without its newline) and give back its body. False if its check
// does not match.
bool parse_log_record(const std::string &line, std::string &body);

class FileStorage : public Storage {
public:
    FileStorage() = default;
    ~FileStorage() override;
    FileStorage(const FileStorage &) = delete;
    FileStorage &operator=(const FileStorage &) = delete;

    // Open `path`, creating it if absent, and replay it into `out`. `report` says
    // what was found. False, with `err`, if it cannot be read or written, or is
    // damaged further back than a crash can reach.
    bool open(const std::string &path, Durable &out, std::string &report, std::string &err);

    bool save(Term term, int voted_for, Index from, const std::vector<Entry> &log) override;

    std::size_t batches() const { return batches_; }      // flushes since open()

private:
    int fd_ = -1;
    Term term_ = 0;             // what the file holds now
    int voted_for_ = NOBODY;
    Index last_ = 0;
    std::size_t batches_ = 0;
};

#endif
