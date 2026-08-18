#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace kvstore::platform {

// The engine is written in POSIX terms -- open/pread/pwrite/fsync/ftruncate on
// an integer descriptor. Windows has none of those, so this is the one place
// that knows the difference. On Linux/macOS every function here is a direct
// passthrough to <unistd.h>; on Windows it is the Win32 equivalent, chosen to
// preserve the semantics the engine actually depends on:
//
//   pread/pwrite  -- positional I/O that does not touch a shared file cursor.
//                    Win32 gets this from ReadFile/WriteFile with an OVERLAPPED
//                    offset. (Not _lseek+_read: that is two steps, and the
//                    cursor is exactly the state we don't want to depend on.)
//   fsync         -- _commit(), which is FlushFileBuffers underneath.
//   O_BINARY      -- Windows only, and mandatory: without it the CRT rewrites
//                    0x0A bytes on the way through and silently corrupts every
//                    record whose key or value happens to contain one.
//
// All functions return -1 (or a negative count) on failure and set errno, so
// callers stay POSIX-shaped and can use last_error() for the message.

// Opens for read+write, creating if absent. Returns a descriptor or -1.
[[nodiscard]] int open_read_write(const std::filesystem::path& path);

int close_fd(int fd);

// Read up to `count` bytes at `offset`. Returns bytes read, 0 at EOF, -1 on
// error. Short reads are legal and the caller must loop.
[[nodiscard]] std::int64_t pread_at(int fd, void* buf, std::size_t count, std::uint64_t offset);

// Write `count` bytes at `offset`. Returns bytes written or -1. Short writes
// are legal and the caller must loop.
[[nodiscard]] std::int64_t pwrite_at(int fd, const void* buf, std::size_t count,
                                     std::uint64_t offset);

// Flush this file's data and metadata all the way to the device.
[[nodiscard]] int fsync_fd(int fd);

[[nodiscard]] int ftruncate_fd(int fd, std::uint64_t length);

// Current size in bytes, or -1.
[[nodiscard]] std::int64_t file_size(int fd);

// Flush a *directory's* own contents -- its list of names -- to the device.
//
// This is the half of "write a temp file, fsync it, rename it into place" that
// is easy to miss. fsync on the temp file makes the file's *bytes* durable. It
// says nothing about the directory entry that gives those bytes a name, and a
// rename only edits the directory. So after a rename, a power cut can leave a
// filesystem that has the new file's data but not its new name -- or worse,
// neither name. fsyncing the directory is what commits the rename.
//
// Windows has no equivalent and needs none here; see the note in the .cpp.
[[nodiscard]] int sync_directory(const std::filesystem::path& dir);

// Atomically replace `to` with `from`. Both must be in the same directory --
// a cross-filesystem move is a copy plus a delete, which is not atomic and is
// precisely the property this call exists for.
//
// After this returns, a reader opening `to` sees either the whole old file or
// the whole new one, never a blend: the rename is a single directory-entry
// update. Call sync_directory() afterwards to make that update durable.
[[nodiscard]] int rename_file(const std::filesystem::path& from,
                              const std::filesystem::path& to);

// strerror(errno) for the call that just failed.
[[nodiscard]] std::string last_error();

}  // namespace kvstore::platform
