#include "kvstore/log_file.hpp"

#include <utility>

#include "platform_file.hpp"

namespace kvstore {

Result<LogFile> LogFile::open(const std::filesystem::path& path, SyncMode mode) {
    const int fd = platform::open_read_write(path);
    if (fd < 0) {
        return Status::io_error("open " + path.string() + ": " + platform::last_error());
    }

    const std::int64_t size = platform::file_size(fd);
    if (size < 0) {
        const std::string err = platform::last_error();
        (void)platform::close_fd(fd);  // Don't leak the fd on the error path.
        return Status::io_error("stat " + path.string() + ": " + err);
    }

    return LogFile{fd, path, static_cast<std::uint64_t>(size), mode};
}

LogFile::~LogFile() { close_if_open(); }

LogFile::LogFile(LogFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      end_offset_(std::exchange(other.end_offset_, 0)),
      sync_mode_(other.sync_mode_) {}

LogFile& LogFile::operator=(LogFile&& other) noexcept {
    if (this != &other) {
        close_if_open();  // We may already own a different fd.
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
        end_offset_ = std::exchange(other.end_offset_, 0);
        sync_mode_ = other.sync_mode_;
    }
    return *this;
}

void LogFile::close_if_open() noexcept {
    if (fd_ >= 0) {
        // Nothing useful to do if close fails, and a destructor must not throw.
        (void)platform::close_fd(fd_);
        fd_ = -1;
    }
}

Status LogFile::check_open(const char* what) const {
    if (fd_ < 0) {
        return Status::io_error(std::string{what} + " on a closed file: " + path_.string());
    }
    return Status::ok();
}

Status LogFile::close() {
    if (fd_ < 0) {
        return Status::ok();  // Idempotent: closing twice is not an error.
    }
    const int rc = platform::close_fd(fd_);
    // Clear the descriptor either way. A failed close does not leave the fd
    // usable, and retrying it would be a double close -- which, once the number
    // is recycled, closes some other file.
    fd_ = -1;
    if (rc != 0) {
        return Status::io_error("close " + path_.string() + ": " + platform::last_error());
    }
    return Status::ok();
}

Result<std::uint64_t> LogFile::append(std::span<const std::uint8_t> bytes) {
    if (Status s = check_open("append"); !s.is_ok()) {
        return s;
    }

    const std::uint64_t offset = end_offset_;

    // pwrite may write fewer bytes than asked (a signal, a full-ish disk), so
    // loop until done. Treating a short write as success is how a log ends up
    // with a half record nobody noticed.
    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::int64_t n = platform::pwrite_at(fd_, bytes.data() + written,
                                                   bytes.size() - written, offset + written);
        if (n < 0) {
            // The file may now hold a partial record. That is fine and is
            // exactly what recovery's crc check is for -- we just must not
            // advance end_offset_ or tell the caller it worked.
            return Status::io_error("write " + path_.string() + ": " + platform::last_error());
        }
        if (n == 0) {
            return Status::io_error("write made no progress on " + path_.string());
        }
        written += static_cast<std::size_t>(n);
    }

    // Only now is the record part of the log.
    end_offset_ += bytes.size();

    if (sync_mode_ == SyncMode::Always) {
        Status s = sync();
        if (!s.is_ok()) {
            return s;
        }
    }
    return offset;
}

Result<std::vector<std::uint8_t>> LogFile::read_at(std::uint64_t offset,
                                                   std::uint32_t size) const {
    if (Status s = check_open("read"); !s.is_ok()) {
        return s;
    }

    std::vector<std::uint8_t> buf(size);

    std::size_t got = 0;
    while (got < buf.size()) {
        const std::int64_t n =
            platform::pread_at(fd_, buf.data() + got, buf.size() - got, offset + got);
        if (n < 0) {
            return Status::io_error("read " + path_.string() + ": " + platform::last_error());
        }
        if (n == 0) {
            // EOF before we got what we asked for. The caller's offset/size came
            // from the index or a record header, so the file is shorter than
            // the database believes: corrupt, not an I/O fault.
            return Status::corruption("short read: " + path_.string() +
                                      " ends inside the record");
        }
        got += static_cast<std::size_t>(n);
    }
    return buf;
}

Status LogFile::sync() {
    KVSTORE_RETURN_IF_ERROR(check_open("fsync"));
    if (platform::fsync_fd(fd_) != 0) {
        return Status::io_error("fsync " + path_.string() + ": " + platform::last_error());
    }
    return Status::ok();
}

Status LogFile::truncate(std::uint64_t length) {
    KVSTORE_RETURN_IF_ERROR(check_open("truncate"));
    if (platform::ftruncate_fd(fd_, length) != 0) {
        return Status::io_error("truncate " + path_.string() + ": " + platform::last_error());
    }
    end_offset_ = length;

    // Truncation is a structural change to the database -- if we cut a torn
    // tail and then crash before it reaches the disk, we would find the same
    // garbage again on the next open. Cheap here because it happens once, at
    // startup.
    return sync();
}

}  // namespace kvstore
