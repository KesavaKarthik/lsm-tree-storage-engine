#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kvstore/file_names.hpp"
#include "kvstore/lsm.hpp"
#include "temp_dir.hpp"

// Helpers shared by the LSM test files, for the same reason store_helpers.hpp
// exists for the Bitcask ones: anything that opens a store, names a file, or
// counts what is in a directory lives in one place, so two suites cannot drift
// into disagreeing about what an LSM database looks like.

namespace kvstore::testing_support {

inline std::filesystem::path table_path(const TempDir& dir, FileId id) {
    return dir.path() / sst_file_name(id);
}

inline std::filesystem::path wal_path(const TempDir& dir, FileId id) {
    return dir.path() / wal_file_name(id);
}

// The SSTables actually present, ascending. What recovery would find.
inline std::vector<FileId> sst_file_ids(const TempDir& dir) {
    std::vector<FileId> ids;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (const auto id = parse_sst_file_id(entry.path().filename().string())) {
            ids.push_back(*id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline std::vector<FileId> wal_file_ids(const TempDir& dir) {
    std::vector<FileId> ids;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (const auto id = parse_wal_file_id(entry.path().filename().string())) {
            ids.push_back(*id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline std::unique_ptr<LsmStore> open_lsm_or_fail(const TempDir& dir, const LsmOptions& options) {
    auto store = LsmStore::open(dir.path(), options);
    EXPECT_TRUE(store.is_ok()) << store.status().to_string();
    if (!store.is_ok()) {
        return nullptr;
    }
    std::unique_ptr<LsmStore> opened = store.take();

    // Settle before handing it over.
    //
    // Recovery can leave levels over budget, so the background thread may start
    // compacting the moment the store exists -- and a test that then counts
    // files on disk is racing it. Every assertion in this suite is about a
    // settled tree, so waiting here once is better than sprinkling waits through
    // thirty tests and forgetting one. A store that halted at a fail point
    // returns immediately.
    (void)opened->wait_for_background();
    return opened;
}

// Never by default: these cases test semantics, not durability, and an fsync per
// put makes the suite slow for no coverage. The durability cases opt into Always.
inline std::unique_ptr<LsmStore> open_lsm_or_fail(const TempDir& dir,
                                                  SyncMode mode = SyncMode::Never) {
    LsmOptions options;
    options.sync_mode = mode;
    return open_lsm_or_fail(dir, options);
}

// A threshold small enough to cross with short strings, and blocks small enough
// that a handful of keys spans several of them -- which is the only way to test
// that the sparse index is doing anything at all.
inline LsmOptions tiny_lsm(std::uint64_t memtable_size, std::uint32_t block_size = 64,
                           LsmFailPoint fail_at = LsmFailPoint::None) {
    LsmOptions options;
    options.sync_mode = SyncMode::Never;
    options.memtable_size = memtable_size;
    options.block_size = block_size;
    options.fail_at = fail_at;
    return options;
}

// Compaction turned off, for the cases that need to *observe* several tables
// sitting side by side -- read amplification, bloom rejections, tombstone
// shadowing across files. Not a production setting; it is how a test says
// "hold still while I count". The budget is high rather than infinite so that
// level_budget()'s repeated multiply cannot overflow.
inline LsmOptions without_compaction(LsmOptions options) {
    options.l0_compaction_trigger = 1000000;
    options.level_base_bytes = 1ull << 40;
    return options;
}

// Budgets small enough that a few hundred short keys build a genuine multi-level
// tree. The production defaults (10MiB at L1, x10 per level) would keep every
// test in this file at L0 forever.
inline LsmOptions leveled(std::uint64_t memtable_size = 256, std::size_t l0_trigger = 2,
                          std::uint64_t level_base = 2048, std::uint64_t target_table = 1024,
                          LsmFailPoint fail_at = LsmFailPoint::None) {
    LsmOptions options;
    options.sync_mode = SyncMode::Never;
    options.memtable_size = memtable_size;
    options.block_size = 64;
    options.l0_compaction_trigger = l0_trigger;
    options.level_base_bytes = level_base;
    options.level_multiplier = 4;
    options.target_table_size = target_table;
    options.fail_at = fail_at;
    return options;
}

inline LsmOptions stacked_tables(SyncMode mode = SyncMode::Never) {
    LsmOptions options;
    options.sync_mode = mode;
    return without_compaction(options);
}

// Drains an iterator into something a test can compare against a literal.
inline std::vector<std::pair<std::string, std::string>> collect(Iterator& it) {
    std::vector<std::pair<std::string, std::string>> out;
    while (it.valid()) {
        out.emplace_back(std::string{it.key()}, std::string{it.value()});
        const Status status = it.next();
        if (!status.is_ok()) {
            ADD_FAILURE() << "iteration failed: " << status.to_string();
            break;
        }
    }
    return out;
}

inline std::vector<std::string> keys_of(const std::vector<std::pair<std::string, std::string>>& v) {
    std::vector<std::string> keys;
    keys.reserve(v.size());
    for (const auto& [key, value] : v) {
        keys.push_back(key);
    }
    return keys;
}

}  // namespace kvstore::testing_support
