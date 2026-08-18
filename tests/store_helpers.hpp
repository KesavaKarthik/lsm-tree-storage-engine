#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/file_names.hpp"
#include "temp_dir.hpp"

// Helpers shared by the Bitcask test files. Anything that opens a store, names
// a file, or reaches behind the engine's back to damage one lives here, so the
// two suites cannot drift into disagreeing about what a database looks like.

namespace kvstore::testing_support {

inline std::filesystem::path data_path(const TempDir& dir, FileId id) {
    return dir.path() / log_file_name(id);
}

inline std::filesystem::path hint_path(const TempDir& dir, FileId id) {
    return dir.path() / hint_file_name(id);
}

inline std::unique_ptr<Bitcask> open_or_fail(const TempDir& dir, const BitcaskOptions& options) {
    auto store = Bitcask::open(dir.path(), options);
    EXPECT_TRUE(store.is_ok()) << store.status().to_string();
    return store.is_ok() ? store.take() : nullptr;
}

inline std::unique_ptr<Bitcask> open_or_fail(const TempDir& dir,
                                             SyncMode mode = SyncMode::Always) {
    return open_or_fail(dir, BitcaskOptions{mode});
}

// Rotation and compaction tests need a threshold small enough to cross with
// short strings, and don't care about durability -- an fsync per put would make
// the suite slow for no coverage.
inline BitcaskOptions tiny_files(std::uint64_t max_file_size,
                                 CompactionFailPoint fail_at = CompactionFailPoint::None) {
    BitcaskOptions options;
    options.sync_mode = SyncMode::Never;
    options.max_file_size = max_file_size;
    options.fail_at = fail_at;
    return options;
}

// The data files actually present, ascending. What recovery would find.
inline std::vector<FileId> data_file_ids(const TempDir& dir) {
    std::vector<FileId> ids;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (const auto id = parse_log_file_id(entry.path().filename().string())) {
            ids.push_back(*id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline std::vector<FileId> hint_file_ids(const TempDir& dir) {
    std::vector<FileId> ids;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (const auto id = parse_hint_file_id(entry.path().filename().string())) {
            ids.push_back(*id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline std::vector<std::string> temp_file_names(const TempDir& dir) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        const std::string name = entry.path().filename().string();
        if (is_temp_name(name)) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Total bytes of every data file -- what the database costs on disk, measured
// from the outside rather than trusting the engine's own accounting.
inline std::uint64_t bytes_on_disk(const TempDir& dir) {
    std::uint64_t total = 0;
    for (const FileId id : data_file_ids(dir)) {
        total += std::filesystem::file_size(data_path(dir, id));
    }
    return total;
}

inline void append_raw(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    ASSERT_TRUE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
}

// Flips every bit of one byte, in place, behind the engine's back.
inline void flip_byte_at(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());

    file.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    ASSERT_TRUE(file.read(&byte, 1));

    byte = static_cast<char>(byte ^ 0xFF);
    file.seekp(static_cast<std::streamoff>(offset));
    ASSERT_TRUE(file.write(&byte, 1));
    file.flush();
}

// Cuts `bytes` off the end of a file, the way a crash mid-write would.
inline void truncate_by(const std::filesystem::path& path, std::uint64_t bytes) {
    const auto size = std::filesystem::file_size(path);
    ASSERT_GE(size, bytes);
    std::filesystem::resize_file(path, size - bytes);
}

// Replaces bytes in place, behind the engine's back. Lets a test substitute a
// *well-formed* structure that is nonetheless wrong -- which is how you isolate
// a semantic check from the crc that would otherwise fire first.
inline void overwrite_at(const std::filesystem::path& path,
                         std::uint64_t offset,
                         const std::vector<std::uint8_t>& bytes) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
    file.flush();
}

}  // namespace kvstore::testing_support
