#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/result.hpp"

namespace kvstore::hint {

// A hint file is an index-only sidecar for one data file: everything recovery
// needs to rebuild the index for that file, without reading the values.
//
// WHY. Recovery's cost is dominated by reading every record of every file --
// including the value bytes, which it then throws away, because the index only
// stores where a value is, not what it is. A hint entry is a key plus three
// numbers, so a hint file is roughly the size of the *keys* in its data file
// rather than the size of the data. Opening a compacted 1GB file of large
// values becomes reading a few megabytes.
//
// THE ONE RULE: A HINT IS A CACHE, NEVER A SOURCE OF TRUTH.
// Missing, short, mismatched, or crc-failing all mean the same thing -- fall
// back to a full scan of the data file. Never an error. Recovery's correctness
// must not depend on a hint being present or being right, and that is what
// makes the crash-safety argument for compaction easy: a hint can be deleted at
// any moment without consequence.
//
// LAYOUT
//
//   entry:  [ crc32 4B | timestamp 8B | record_offset 8B | record_size 4B | key_size 4B | key ]
//           \_________/ \____________________ covered by the crc ______________________/
//
//   footer: [ magic "KVHT" 4B | data_file_size 8B | entry_count 8B | crc32 4B ]
//
// Deliberate choices, and what each buys:
//
//   * The entry is shaped like a record header -- crc first because it cannot
//     cover itself, little-endian, byte-at-a-time. Two formats in one engine
//     should not disagree about how to write down an integer.
//
//   * record_size is stored rather than derived from key_size and value_size,
//     so replaying a hint never needs to know the record format at all. The
//     hint is self-contained; that is the point of it.
//
//   * There is no flags byte, because a compacted data file contains no
//     tombstones -- compaction drops them. If partial compaction ever has to
//     retain tombstones (see compaction.cpp), this format needs the byte back.
//
//   * THE FOOTER IS THE HINT'S COMMIT RECORD. A hint with no valid footer is an
//     incomplete hint and is ignored wholesale. Since a hint is only ever
//     installed by the same fsync-then-rename dance as its data file, a torn
//     one should be impossible -- the footer costs 24 bytes to turn that
//     "impossible" into a "detected".
//
//   * data_file_size binds the hint to its data file. If a hint were ever to
//     outlive the file it describes, its offsets would point into unrelated
//     bytes; comparing against the real file length costs one stat and rules
//     that out. Belt and braces on top of the install ordering, which already
//     removes a stale hint before its data file changes.

inline constexpr std::size_t kEntryHeaderSize = 28;  // 4 + 8 + 8 + 4 + 4
inline constexpr std::size_t kFooterSize = 24;       // 4 + 8 + 8 + 4
inline constexpr std::uint8_t kMagic[4] = {'K', 'V', 'H', 'T'};

struct Entry {
    std::uint64_t timestamp = 0;
    std::uint64_t record_offset = 0;  // Where the record starts in the data file.
    std::uint32_t record_size = 0;    // Total record size, header included.
    std::string key;

    // Bytes this entry occupies in the hint file.
    [[nodiscard]] std::uint64_t encoded_size() const noexcept {
        return kEntryHeaderSize + key.size();
    }
};

struct Footer {
    std::uint64_t data_file_size = 0;
    std::uint64_t entry_count = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_entry(std::string_view key,
                                                     std::uint64_t timestamp,
                                                     std::uint64_t record_offset,
                                                     std::uint32_t record_size);

// One entry from the front of `bytes`, crc verified. `bytes` may be longer;
// anything past the entry is ignored. Corruption if short or mis-shaped.
[[nodiscard]] Result<Entry> decode_entry(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_footer(std::uint64_t data_file_size,
                                                      std::uint64_t entry_count);

// Expects exactly the last kFooterSize bytes of a hint file.
[[nodiscard]] Result<Footer> decode_footer(std::span<const std::uint8_t> bytes);

}  // namespace kvstore::hint
