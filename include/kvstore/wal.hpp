#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "kvstore/log_file.hpp"
#include "kvstore/memtable.hpp"
#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// The write-ahead log: the durability half of a write whose other half is a
// insertion into RAM.
//
// **How this differs from Bitcask's log, which is the whole point of the
// phase.** In Bitcask the append-only log *is* the database. The record a put()
// appends is the record a get() reads back, years later; the in-memory index
// holds only a pointer to it. The log is permanent storage, and compaction
// exists to rewrite it because it is the only copy.
//
// Here the WAL is a crash-recovery scratchpad and nothing else. Nobody ever
// reads it except recovery. It is unsorted and unindexed, so it could not answer
// a get() even if asked. Its entire job is to make one promise survive a power
// cut: *this write reached the memtable*. The moment the memtable it mirrors is
// flushed to an SSTable, the whole file is deleted, unread. Bitcask's log is
// durable storage; this is a durable intent to have inserted into RAM.
//
// The consequence is that the format costs nothing to design: it is
// record::encode(), byte for byte -- same crc32, same timestamp, same tombstone
// bit -- and replay is scan_records(), which already knows how to stop at a torn
// tail. Reusing a proven format for a file with completely different semantics is
// the cheapest correct thing available, and the reason it is safe is that the two
// files agree about what bytes mean while disagreeing about what they are for.
class Wal {
public:
    // Opens (creating if needed) and positions at the end.
    [[nodiscard]] static Result<Wal> open(const std::filesystem::path& path, SyncMode mode);

    Wal(Wal&&) noexcept = default;
    Wal& operator=(Wal&&) noexcept = default;
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Appends one write. Under SyncMode::Always this returns only once the entry
    // is on stable storage, which is what makes it legal for the caller to
    // acknowledge the write immediately afterwards.
    [[nodiscard]] Status append(std::string_view key, std::string_view value, bool tombstone);

    // Rebuilds `into` from this file, applying entries in the order they were
    // written so later writes overwrite earlier ones. Returns the number of
    // entries replayed.
    //
    // A partial entry at the end is expected, not an error: it is the record that
    // was mid-append when the machine stopped, and the write it belongs to was
    // never acknowledged. It is truncated away, exactly as Bitcask truncates the
    // torn tail of its active file. Unlike Bitcask there is no "sealed" case to
    // distinguish -- every WAL is the active one, so damage anywhere but the tail
    // is the only thing that could be reported, and a crc failure mid-file cuts
    // the log there and loses the writes after it. That is a real (if remote)
    // exposure and is written down rather than papered over: the alternative,
    // refusing to open, would turn one bad sector into a database nobody can
    // start.
    [[nodiscard]] Result<std::uint64_t> replay_into(Memtable* into);

    [[nodiscard]] Status sync();

    // Releases the descriptor early. Required before the file can be renamed or
    // unlinked on Windows -- see LogFile::close().
    [[nodiscard]] Status close();

    [[nodiscard]] std::uint64_t size() const noexcept { return file_.size(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return file_.path(); }

private:
    explicit Wal(LogFile file) : file_(std::move(file)) {}

    LogFile file_;
};

}  // namespace kvstore
