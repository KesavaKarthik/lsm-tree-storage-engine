#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "kvstore/result.hpp"
#include "kvstore/status.hpp"
#include "platform_file.hpp"

namespace kvstore {

// Owns one read-only memory mapping of a whole file. Move-only, for the same
// reason LogFile is: two objects unmapping the same address is a double free
// with a nicer name.
//
// **It holds no file descriptor.** open() opens the file, maps it, and closes
// the descriptor immediately -- the mapping keeps its own reference to the file
// on both platforms, so the fd has nothing left to do. That is worth knowing
// rather than assuming: a mapped table costs an address range and no descriptor,
// so the descriptor limit that shaped Bitcask's 64MiB file size (see
// bitcask.hpp) simply does not apply to tables read this way.
//
// Private to src/ on purpose. SSTable holds one behind a unique_ptr and
// forward-declares it, so <sys/mman.h> and <windows.h> stay out of every
// translation unit that includes a public header.
//
// Only ever used on SSTables, and only because they are immutable. A mapping of
// a file somebody is still appending to has no defined size, and a mapping of a
// file somebody might truncate turns a read into SIGBUS. Neither can happen to a
// table: it is renamed into place complete and never touched again.
class MappedFile {
public:
    // Fails with IOError if the file cannot be opened or mapped, and with
    // InvalidArgument for an empty file -- neither platform can map zero bytes,
    // and no real table is empty (a footer alone is 44).
    [[nodiscard]] static Result<MappedFile> open(const std::filesystem::path& path);

    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // The whole file. Valid until this object is closed or destroyed.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {mapping_.data, mapping_.size};
    }

    [[nodiscard]] std::size_t size() const noexcept { return mapping_.size; }
    [[nodiscard]] bool is_mapped() const noexcept { return mapping_.is_valid(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Releases the mapping early, before the destructor would. Idempotent.
    //
    // Required before the file can be unlinked on Windows, which will not remove
    // a file with a live mapping -- exactly the rule that already applies to an
    // open handle, and the reason SSTable's destructor unmaps before it deletes.
    [[nodiscard]] Status close();

    // Advises the kernel that access will be random, so it stops reading ahead.
    // Best-effort: getting this wrong costs performance, never correctness.
    [[nodiscard]] Status advise_random();

private:
    MappedFile(platform::Mapping mapping, std::filesystem::path path)
        : mapping_(mapping), path_(std::move(path)) {}

    void unmap_if_mapped() noexcept;

    platform::Mapping mapping_;
    std::filesystem::path path_;
};

}  // namespace kvstore
