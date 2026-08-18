#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// How hard append() tries before it reports success.
//
// The path a write takes: pwrite() copies bytes into the OS page cache and
// returns. At that moment the data survives the *process* dying -- another
// reader would see it -- but not the *machine* dying, because it may still be
// only in RAM. fsync() is what pushes it to the device and waits.
//
//   Always -- fsync on every append. A returned Ok means the record is on
//             stable storage. Costs a device round trip per write (milliseconds
//             on a spinning disk, tens of microseconds on an SSD).
//   Never  -- let the OS flush when it feels like it. Fast, and a power cut
//             loses whatever was still in the page cache. The log stays
//             *valid* either way -- recovery drops the torn tail -- so this
//             trades durability, not integrity.
//
// (fdatasync, where it exists, is the cheaper cousin: it flushes the data but
// skips metadata that doesn't affect reading it back, e.g. mtime. It still
// flushes a size change, which every append is, so for this workload it saves
// little. Using fsync and keeping it obvious.)
enum class SyncMode {
    Always,
    Never,
};

// Owns one open file descriptor. Move-only: two objects must never hold the
// same fd, or the first destructor closes a descriptor the second still uses --
// and a recycled fd number means the second one's writes land in whatever file
// opened next. RAII means the close happens on every exit path, including an
// exception thrown past this scope, without a single explicit close() call.
class LogFile {
public:
    // Opens (creating if needed) and seeks to the end. Returns IOError with
    // errno's message on failure.
    [[nodiscard]] static Result<LogFile> open(const std::filesystem::path& path, SyncMode mode);

    ~LogFile();

    LogFile(LogFile&& other) noexcept;
    LogFile& operator=(LogFile&& other) noexcept;

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    // Appends at the current end and returns the offset it landed at -- which
    // is what the index stores. fsyncs afterwards if SyncMode::Always.
    //
    // A span rather than a vector so compaction can hand over the bytes it just
    // read from an input file without copying them into a fresh buffer first.
    [[nodiscard]] Result<std::uint64_t> append(std::span<const std::uint8_t> bytes);

    // Exactly `size` bytes at `offset`. A short read means the file is smaller
    // than the caller believed, which is Corruption, not IOError.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_at(std::uint64_t offset,
                                                            std::uint32_t size) const;

    [[nodiscard]] Status sync();

    // Drops everything past `length`. Used by recovery to cut off a torn tail.
    [[nodiscard]] Status truncate(std::uint64_t length);

    // Releases the descriptor early, before the destructor would.
    //
    // Needed because Windows will not let this process rename over or unlink a
    // file it still holds open -- open_read_write asks for _SH_DENYNO, which
    // shares read and write access but not delete. Compaction therefore has to
    // close a file before installing a new one over its name. Idempotent, and
    // every other operation returns IOError afterwards rather than handing -1
    // to a syscall.
    [[nodiscard]] Status close();

    // Bytes written so far == the offset the next append will get.
    [[nodiscard]] std::uint64_t size() const noexcept { return end_offset_; }

    // Where this file lives. Compaction renames and unlinks files, and every
    // I/O error message is more useful with a filename in it.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

private:
    LogFile(int fd, std::filesystem::path path, std::uint64_t end_offset, SyncMode mode)
        : fd_(fd), path_(std::move(path)), end_offset_(end_offset), sync_mode_(mode) {}

    void close_if_open() noexcept;

    // Every entry point checks this. A closed LogFile is a programming error,
    // not a data condition, but passing -1 to pwrite would be a silent one.
    [[nodiscard]] Status check_open(const char* what) const;

    int fd_ = -1;  // -1 means "moved from" or "closed"; both must be tolerated.
    std::filesystem::path path_;
    std::uint64_t end_offset_ = 0;
    SyncMode sync_mode_ = SyncMode::Always;
};

}  // namespace kvstore
