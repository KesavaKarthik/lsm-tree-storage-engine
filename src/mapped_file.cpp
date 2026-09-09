#include "mapped_file.hpp"

#include <system_error>
#include <utility>

namespace kvstore {

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    // Checked before opening, because platform::open_read_write() *creates* what
    // it cannot find -- the same trap SSTable::open() steps around.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Status::io_error("no such file to map: " + path.string());
    }

    const int fd = platform::open_read_write(path);
    if (fd < 0) {
        return Status::io_error("open " + path.string() + ": " + platform::last_error());
    }

    const std::int64_t size = platform::file_size(fd);
    if (size < 0) {
        const std::string message = platform::last_error();
        (void)platform::close_fd(fd);
        return Status::io_error("stat " + path.string() + ": " + message);
    }
    if (size == 0) {
        (void)platform::close_fd(fd);
        return Status::invalid_argument("cannot map an empty file: " + path.string());
    }

    platform::Mapping mapping;
    if (platform::map_readonly(fd, static_cast<std::uint64_t>(size), &mapping) != 0) {
        const std::string message = platform::last_error();
        (void)platform::close_fd(fd);
        return Status::io_error("mmap " + path.string() + ": " + message);
    }

    // The descriptor has done its job. Both mmap() and CreateFileMapping take
    // their own reference to the file, so the mapping stays valid -- and a table
    // held open this way costs no descriptor at all.
    (void)platform::close_fd(fd);

    return MappedFile{mapping, path};
}

MappedFile::~MappedFile() { unmap_if_mapped(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : mapping_(std::exchange(other.mapping_, platform::Mapping{})),
      path_(std::move(other.path_)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        unmap_if_mapped();  // We may already own a different mapping.
        mapping_ = std::exchange(other.mapping_, platform::Mapping{});
        path_ = std::move(other.path_);
    }
    return *this;
}

void MappedFile::unmap_if_mapped() noexcept {
    if (mapping_.is_valid()) {
        (void)platform::unmap(mapping_);
    }
    mapping_ = platform::Mapping{};
}

Status MappedFile::close() {
    if (!mapping_.is_valid()) {
        return {};  // Idempotent, like LogFile::close().
    }
    // Clear the handle regardless of the outcome. A mapping we failed to release
    // is not a mapping we may try to release again, and leaving a stale pointer
    // behind for a later destructor is how one failure becomes two.
    const int rc = platform::unmap(mapping_);
    mapping_ = platform::Mapping{};
    if (rc != 0) {
        return Status::io_error("munmap " + path_.string() + ": " + platform::last_error());
    }
    return {};
}

Status MappedFile::advise_random() {
    if (!mapping_.is_valid()) {
        return Status::io_error("advise on a file that is not mapped: " + path_.string());
    }
    if (platform::advise_random(mapping_) != 0) {
        return Status::io_error("madvise " + path_.string() + ": " + platform::last_error());
    }
    return {};
}

}  // namespace kvstore
