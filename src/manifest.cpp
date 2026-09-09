#include "kvstore/manifest.hpp"

#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#include "encoding.hpp"
#include "kvstore/log_file.hpp"
#include "kvstore/record.hpp"
#include "platform_file.hpp"

namespace kvstore::manifest {

namespace {

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    encoding::put_u32(out, at, v);
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    encoding::put_u64(out, at, v);
}

void append_string(std::vector<std::uint8_t>& out, const std::string& s) {
    append_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

// Reads a length-prefixed string, bounding the declared length against what is
// actually left before believing it.
Status take_string(std::span<const std::uint8_t> bytes, std::size_t limit, std::size_t* at,
                   std::string* out) {
    if (limit - *at < 4) {
        return Status::corruption("manifest ends inside a key length");
    }
    const std::uint32_t size = encoding::get_u32(bytes, *at);
    *at += 4;
    if (size > limit - *at) {
        return Status::corruption("manifest key claims more bytes than the file holds");
    }
    out->assign(reinterpret_cast<const char*>(bytes.data() + *at), size);
    *at += size;
    return {};
}

}  // namespace

std::size_t LevelSet::table_count() const noexcept {
    std::size_t count = 0;
    for (const auto& level : levels) {
        count += level.size();
    }
    return count;
}

std::uint64_t LevelSet::level_bytes(std::size_t level) const noexcept {
    if (level >= levels.size()) {
        return 0;
    }
    std::uint64_t bytes = 0;
    for (const TableMeta& table : levels[level]) {
        bytes += table.file_size;
    }
    return bytes;
}

std::uint64_t LevelSet::total_bytes() const noexcept {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        bytes += level_bytes(i);
    }
    return bytes;
}

std::vector<std::uint8_t> encode(const LevelSet& set) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    append_u32(out, kFormatVersion);
    append_u32(out, set.next_id);
    append_u32(out, static_cast<std::uint32_t>(set.levels.size()));

    for (const auto& level : set.levels) {
        append_u32(out, static_cast<std::uint32_t>(level.size()));
        for (const TableMeta& table : level) {
            append_u32(out, table.id);
            append_u64(out, table.file_size);
            append_u64(out, table.entry_count);
            append_string(out, table.min_key);
            append_string(out, table.max_key);
        }
    }

    const std::uint32_t crc = record::crc32_of(out);
    append_u32(out, crc);
    return out;
}

Result<LevelSet> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize + kTrailerSize) {
        return Status::corruption("manifest is too small to be one");
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return Status::corruption("not a manifest: bad magic");
    }

    const std::size_t payload_size = bytes.size() - kTrailerSize;
    if (encoding::get_u32(bytes, payload_size) !=
        record::crc32_of(bytes.subspan(0, payload_size))) {
        return Status::corruption("manifest fails its crc");
    }

    const std::uint32_t version = encoding::get_u32(bytes, 4);
    if (version != kFormatVersion) {
        return Status::corruption("unsupported manifest format version " +
                                  std::to_string(version));
    }

    LevelSet set;
    set.next_id = encoding::get_u32(bytes, 8);
    const std::uint32_t level_count = encoding::get_u32(bytes, 12);

    // Every count below came off disk. The crc says these are the bytes we
    // wrote; it does not say they describe a sane structure, and a corrupt
    // level_count is otherwise an instruction to allocate four billion vectors.
    if (static_cast<std::uint64_t>(level_count) * 4 > payload_size - kHeaderSize) {
        return Status::corruption("manifest declares more levels than it could hold");
    }
    set.levels.resize(level_count);

    std::size_t at = kHeaderSize;
    for (std::uint32_t level = 0; level < level_count; ++level) {
        if (payload_size - at < 4) {
            return Status::corruption("manifest ends inside a level header");
        }
        const std::uint32_t table_count = encoding::get_u32(bytes, at);
        at += 4;
        if (static_cast<std::uint64_t>(table_count) * kTableFixedSize > payload_size - at) {
            return Status::corruption("manifest declares more tables than it could hold");
        }
        set.levels[level].reserve(table_count);

        for (std::uint32_t i = 0; i < table_count; ++i) {
            if (payload_size - at < 20) {  // id + file_size + entry_count
                return Status::corruption("manifest ends inside a table entry");
            }
            TableMeta table;
            table.id = encoding::get_u32(bytes, at);
            at += 4;
            table.file_size = encoding::get_u64(bytes, at);
            at += 8;
            table.entry_count = encoding::get_u64(bytes, at);
            at += 8;
            KVSTORE_RETURN_IF_ERROR(take_string(bytes, payload_size, &at, &table.min_key));
            KVSTORE_RETURN_IF_ERROR(take_string(bytes, payload_size, &at, &table.max_key));

            if (table.id < kFirstFileId) {
                return Status::corruption("manifest names a table with an impossible id");
            }
            if (table.max_key < table.min_key) {
                return Status::corruption("manifest names a table whose range is inverted");
            }
            set.levels[level].push_back(std::move(table));
        }
    }

    if (at != payload_size) {
        return Status::corruption("manifest has trailing bytes after its last table");
    }
    return set;
}

Result<std::optional<LevelSet>> read(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / std::string{kManifestName};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Not an error. Either a brand-new database, or one written before
        // manifests existed -- and only the caller can tell those apart.
        return std::optional<LevelSet>{};
    }

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Status::io_error("stat " + path.string() + ": " + ec.message());
    }
    if (size < kHeaderSize + kTrailerSize || size > kMaxManifestSize) {
        return Status::corruption("manifest is not a plausible size: " + path.string());
    }

    auto file = LogFile::open(path, SyncMode::Never);
    if (!file.is_ok()) {
        return file.status();
    }
    auto bytes = file->read_at(0, static_cast<std::uint32_t>(size));
    if (!bytes.is_ok()) {
        return bytes.status();
    }

    auto set = decode(*bytes);
    if (!set.is_ok()) {
        return set.status();
    }
    return std::optional<LevelSet>{set.take()};
}

Status write_atomic(const std::filesystem::path& dir, const LevelSet& set) {
    const std::string final_name{kManifestName};
    const std::filesystem::path final_path = dir / final_name;
    const std::filesystem::path temp_path = dir / temp_name(final_name);

    // LogFile::open() positions at the end of an existing file, so a leftover
    // temp would be appended to rather than replaced -- the same trap
    // SSTableBuilder::create() steps around, for the same reason.
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);

    auto file = LogFile::open(temp_path, SyncMode::Never);
    if (!file.is_ok()) {
        return file.status();
    }

    const std::vector<std::uint8_t> bytes = encode(set);
    auto offset = file->append(bytes);
    if (!offset.is_ok()) {
        return offset.status();
    }

    // fsync the bytes, then close (Windows will not rename over an open file),
    // then rename, then fsync the directory to commit the name itself. Every one
    // of those four steps is load-bearing; this is the commit point.
    KVSTORE_RETURN_IF_ERROR(file->sync());
    KVSTORE_RETURN_IF_ERROR(file->close());

    if (platform::rename_file(temp_path, final_path) != 0) {
        return Status::io_error("rename " + temp_path.string() + " to " + final_path.string() +
                                ": " + platform::last_error());
    }
    if (platform::sync_directory(dir) != 0) {
        return Status::io_error("fsync directory " + dir.string() + ": " +
                                platform::last_error());
    }
    return {};
}

}  // namespace kvstore::manifest
