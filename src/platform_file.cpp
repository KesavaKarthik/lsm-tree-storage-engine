#include "platform_file.hpp"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
// <windows.h> must come after the CRT headers above.
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kvstore::platform {

#ifdef _WIN32
namespace {

HANDLE handle_for(int fd) {
    const auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
    }
    return h;
}

// Win32 reports through GetLastError, the engine reads errno. Translate the
// cases a storage engine actually distinguishes; everything else is EIO.
void set_errno_from_win32(DWORD err) {
    switch (err) {
        case ERROR_ACCESS_DENIED:    errno = EACCES; break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:   errno = ENOENT; break;
        case ERROR_DISK_FULL:        errno = ENOSPC; break;
        case ERROR_INVALID_HANDLE:   errno = EBADF; break;
        case ERROR_HANDLE_EOF:       errno = 0; break;
        default:                     errno = EIO; break;
    }
}

OVERLAPPED overlapped_at(std::uint64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return ov;
}

}  // namespace

int open_read_write(const std::filesystem::path& path) {
    int fd = -1;
    // _SH_DENYNO: other handles may open the file too. The tests rely on it to
    // corrupt a record behind the engine's back, and it matches POSIX, which
    // does no locking by default either.
    const errno_t rc = _wsopen_s(&fd, path.wstring().c_str(),
                                 _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYNO,
                                 _S_IREAD | _S_IWRITE);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return fd;
}

int close_fd(int fd) { return _close(fd); }

std::int64_t pread_at(int fd, void* buf, std::size_t count, std::uint64_t offset) {
    const HANDLE h = handle_for(fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    OVERLAPPED ov = overlapped_at(offset);
    DWORD got = 0;
    if (!::ReadFile(h, buf, static_cast<DWORD>(count), &got, &ov)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            return 0;  // Reading at or past EOF is not an error, just nothing.
        }
        set_errno_from_win32(err);
        return -1;
    }
    return static_cast<std::int64_t>(got);
}

std::int64_t pwrite_at(int fd, const void* buf, std::size_t count, std::uint64_t offset) {
    const HANDLE h = handle_for(fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    OVERLAPPED ov = overlapped_at(offset);
    DWORD put = 0;
    if (!::WriteFile(h, buf, static_cast<DWORD>(count), &put, &ov)) {
        set_errno_from_win32(::GetLastError());
        return -1;
    }
    return static_cast<std::int64_t>(put);
}

int fsync_fd(int fd) { return _commit(fd); }

int ftruncate_fd(int fd, std::uint64_t length) {
    const errno_t rc = _chsize_s(fd, static_cast<__int64>(length));
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

std::int64_t file_size(int fd) { return _filelengthi64(fd); }

int sync_directory(const std::filesystem::path& dir) {
    // Deliberately a no-op, not an oversight.
    //
    // There is no way to flush a directory's name list on Windows the way
    // fsync(dirfd) does on POSIX: FlushFileBuffers wants a file or volume
    // handle, and a directory handle (which needs FILE_FLAG_BACKUP_SEMANTICS
    // to obtain at all) is not one. NTFS journals metadata operations, and the
    // rename below asks for MOVEFILE_WRITE_THROUGH, which does not return
    // until the change is on the device -- so the guarantee POSIX gets from
    // the directory fsync is already covered by the rename itself.
    (void)dir;
    return 0;
}

int rename_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    // MOVEFILE_REPLACE_EXISTING: the destination normally exists -- compaction
    //   installs its output over the highest input id.
    // MOVEFILE_WRITE_THROUGH: don't return until the change has reached the
    //   device. This is what stands in for POSIX's directory fsync.
    // MOVEFILE_COPY_ALLOWED is deliberately *not* set: it would let a
    //   cross-volume move silently degrade into a copy-then-delete, which is
    //   not atomic. We only ever rename within one directory, so a failure
    //   here is a bug worth hearing about rather than papering over.
    //
    // Note this fails with ERROR_SHARING_VIOLATION if we still hold `to` open:
    // open_read_write uses _SH_DENYNO, which grants FILE_SHARE_READ|WRITE but
    // not FILE_SHARE_DELETE. Callers must close their handle to the
    // destination first.
    if (!::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_errno_from_win32(::GetLastError());
        return -1;
    }
    return 0;
}

#else  // POSIX

int open_read_write(const std::filesystem::path& path) {
    // 0644: the database is readable by the owner's group and world but only
    // writable by the owner, matching what other daemons do with their data.
    return ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
}

int close_fd(int fd) { return ::close(fd); }

std::int64_t pread_at(int fd, void* buf, std::size_t count, std::uint64_t offset) {
    return ::pread(fd, buf, count, static_cast<off_t>(offset));
}

std::int64_t pwrite_at(int fd, const void* buf, std::size_t count, std::uint64_t offset) {
    return ::pwrite(fd, buf, count, static_cast<off_t>(offset));
}

int fsync_fd(int fd) { return ::fsync(fd); }

int ftruncate_fd(int fd, std::uint64_t length) {
    return ::ftruncate(fd, static_cast<off_t>(length));
}

std::int64_t file_size(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(st.st_size);
}

int sync_directory(const std::filesystem::path& dir) {
    // A directory is a file whose contents are its list of names, so it has an
    // fd and it can be fsynced. O_RDONLY is correct and required -- a directory
    // cannot be opened for writing.
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    const int rc = ::fsync(fd);
    const int saved_errno = errno;  // close() may overwrite it on the way out.
    (void)::close(fd);
    errno = saved_errno;
    return rc;
}

int rename_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    // rename(2) is atomic by specification: the destination name refers to the
    // old file or the new one at every instant, never to a partial state, and
    // never to nothing. Any process holding the old file open keeps reading the
    // old inode, which stays alive until the last descriptor closes.
    return ::rename(from.c_str(), to.c_str());
}

#endif

#ifndef _WIN32
namespace {

// strerror_r has two incompatible signatures and which one you get depends on
// the feature-test macros in effect:
//
//   XSI:  int   strerror_r(int, char*, size_t)   -- 0 on success, fills buf
//   GNU:  char* strerror_r(int, char*, size_t)   -- returns the message, which
//                                                   may not be buf at all
//
// Rather than guess with #ifdef __GLIBC__ && _GNU_SOURCE -- which is fragile
// and silently compiles to the wrong reading of the return value when it guesses
// wrong -- overload on the return type and let the compiler pick. Whichever
// strerror_r this platform declares, exactly one of these is viable.
//
// [[maybe_unused]] on both, because exactly one of them is: the platform
// declares one variant and the other overload is never called. That is the
// design working, not dead code, and without this glibc builds warn about the
// XSI overload on every translation unit that includes this.
[[maybe_unused]] std::string strerror_result(int rc, const char* buf) {
    return rc == 0 ? std::string{buf} : std::string{"unknown error"};
}
[[maybe_unused]] std::string strerror_result(const char* msg, const char*) {
    return msg != nullptr ? std::string{msg} : std::string{"unknown error"};
}

}  // namespace
#endif

std::string last_error() {
    const int err = errno;
    if (err == 0) {
        return "unknown error";
    }
#ifdef _WIN32
    // MSVC deprecates strerror. strerror_s writes into the caller's buffer, so
    // it is already thread-safe.
    char buf[256] = {};
    if (::strerror_s(buf, sizeof(buf), err) != 0) {
        return "errno " + std::to_string(err);
    }
    return buf;
#else
    // Not std::strerror: it returns a pointer to a single static buffer shared
    // by the whole process. Phase 3 puts a thread on every connection, so two of
    // them can format an I/O error at the same moment and read each other's
    // half-written message -- or worse, read one while the other frees it.
    // strerror_r writes into a buffer we own, which is why it exists.
    char buf[256] = {};
    return strerror_result(::strerror_r(err, buf, sizeof(buf)), buf);
#endif
}

}  // namespace kvstore::platform
