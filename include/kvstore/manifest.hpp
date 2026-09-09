#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kvstore/file_names.hpp"
#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore::manifest {

// The manifest: which tables exist, and which level each one lives at.
//
// **The rename of this file is the only commit point in the engine.** Table
// files are written and fsynced before it and deleted only after it, so the one
// question "does the manifest name this table?" answers every crash:
//
//   - Died before the commit? The new outputs are not named. They are orphans.
//   - Died after it? The old inputs are no longer named. They are orphans.
//
// So recovery needs exactly one rule -- *delete every .sst the manifest does not
// name* -- and it cleans up both directions without knowing which happened. That
// single rule is the reason this file exists at all; without it, a compaction
// that adds one table and removes three has no atomic moment, and a crash in the
// middle leaves data either duplicated or gone.
//
// Written whole and replaced atomically (MANIFEST.tmp -> fsync -> rename ->
// fsync the directory), which is the same idiom the Bitcask compaction already
// uses. LevelDB instead appends VersionEdit records to a log and names the live
// one in a CURRENT file, which avoids rewriting the whole state per change --
// that matters at terabytes. Here the level set is a few kilobytes, and a
// whole-state rewrite avoids inventing a second crash-consistency problem
// (is CURRENT durable before the manifest it names?) to solve the first one.
//
//   [ magic "KVMF" 4B | format_version 4B | next_id 4B | level_count 4B ]
//   per level:  [ table_count 4B ]
//     per table:  [ id 4B | file_size 8B | entry_count 8B
//                 | min_key_size 4B | min_key | max_key_size 4B | max_key ]
//   [ crc32 4B ]
//
// Magic goes first here, unlike the SSTable footer where it goes last, because
// this file is read from the front and that is where a reader looks.

inline constexpr std::uint8_t kMagic[4] = {'K', 'V', 'M', 'F'};
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kTrailerSize = 4;
inline constexpr std::size_t kTableFixedSize = 28;  // id + file_size + entry_count + 2 sizes

// A sane ceiling. The manifest is metadata about files, not data, so anything
// this large means a corrupt length rather than a big database.
inline constexpr std::uint64_t kMaxManifestSize = 64ull << 20;

// What the manifest records about one table. Deliberately enough to plan a
// compaction without opening a single file: the picker needs key ranges to
// compute overlaps and sizes to compare a level against its budget.
struct TableMeta {
    FileId id = 0;
    std::uint64_t file_size = 0;
    std::uint64_t entry_count = 0;
    std::string min_key;
    std::string max_key;

    // Half-open ranges would be wrong here: min_key and max_key are both keys
    // that exist in the table, so the comparison is inclusive at both ends.
    [[nodiscard]] bool overlaps(std::string_view begin, std::string_view end) const {
        return !(max_key < begin || end < min_key);
    }
};

// The whole durable state of the tree.
struct LevelSet {
    // Persisted rather than re-derived from filenames, so an orphan that
    // recovery sweeps can never hand its id to a different table later.
    FileId next_id = kFirstFileId;

    // levels[0] is L0, whose tables may overlap; levels[i] for i > 0 hold
    // tables sorted by min_key with disjoint ranges.
    std::vector<std::vector<TableMeta>> levels;

    [[nodiscard]] std::size_t table_count() const noexcept;
    [[nodiscard]] std::uint64_t level_bytes(std::size_t level) const noexcept;
    [[nodiscard]] std::uint64_t total_bytes() const noexcept;
};

[[nodiscard]] std::vector<std::uint8_t> encode(const LevelSet& set);
[[nodiscard]] Result<LevelSet> decode(std::span<const std::uint8_t> bytes);

// Reads `dir`/MANIFEST. An absent manifest is **not an error**: it means either a
// brand-new database or one written before manifests existed, and the caller
// decides which. Returns nullopt in that case.
[[nodiscard]] Result<std::optional<LevelSet>> read(const std::filesystem::path& dir);

// Writes the level set and makes it durable: MANIFEST.tmp -> fsync -> rename ->
// fsync the directory. When this returns Ok, the new state is committed and the
// caller may start deleting the files it no longer names.
[[nodiscard]] Status write_atomic(const std::filesystem::path& dir, const LevelSet& set);

}  // namespace kvstore::manifest
