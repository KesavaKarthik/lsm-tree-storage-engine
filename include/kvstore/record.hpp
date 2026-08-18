#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/result.hpp"

namespace kvstore::record {

// On-disk record layout, all integers little-endian:
//
//   [ crc32 4B | timestamp 8B | flags 1B | key_size 4B | value_size 4B | key | value ]
//   \_________/ \_________________________ covered by crc ________________________/
//
// Why each field:
//   crc32      -- the record's own proof that it is intact. Covers everything
//                 after itself, header included: a crash mid-append leaves a
//                 partial record whose bytes are self-inconsistent, and a bad
//                 disk sector flips bytes silently. Without this we would trust
//                 both and hand back wrong data. It cannot cover itself, hence
//                 it sits first and is excluded.
//   timestamp  -- which of two records for the same key is newer. The log's
//                 order already says so for a single file, but compaction and
//                 rotation reorder records across files, so the answer has to
//                 be *in* the record.
//   flags      -- bit 0 marks a tombstone. A delete has to be a record like any
//                 other: an append-only file cannot go back and erase, so
//                 "deleted" is written down and applied during replay.
//   key_size   -- keys are bytes, not C strings, so the length is explicit.
//                 May not be 0.
//   value_size -- same, and 0 for a tombstone (no value bytes follow).
//
// How a reader advances: the two size fields are at fixed offsets, so after
// reading the 21-byte header it knows the total is 21 + key_size + value_size
// and where the next record starts. That is the only framing there is -- no
// separators, no length prefix at the end. It also means a corrupt header can
// claim any size at all, so a reader must bound those numbers against the real
// file size *before* trusting them (see Bitcask::recover).

inline constexpr std::size_t kCrcSize = 4;
inline constexpr std::size_t kHeaderSize = 21;  // 4 + 8 + 1 + 4 + 4

inline constexpr std::uint8_t kFlagTombstone = 0x01;

// A record's total on-disk size must fit in a uint32_t. Not a format limit but a
// plumbing one: ValuePointer stores the size per key as a uint32_t and
// LogFile::read_at takes a uint32_t. The two size fields are independently 32-bit,
// so their sum can reach ~8.6GB and overflow that -- hence an explicit cap.
//
// Enforced on both sides: when encoding, so a record that could never be read
// back cannot be written; and in decode_header, so a corrupt header cannot smuggle
// an oversized one past a reader and silently narrow it.
inline constexpr std::uint64_t kMaxRecordSize = std::numeric_limits<std::uint32_t>::max();

struct Header {
    std::uint32_t crc = 0;
    std::uint64_t timestamp = 0;
    std::uint8_t flags = 0;
    std::uint32_t key_size = 0;
    std::uint32_t value_size = 0;

    [[nodiscard]] bool is_tombstone() const noexcept { return (flags & kFlagTombstone) != 0; }

    // Total bytes this record occupies on disk. uint64 because the two sizes
    // come off disk unvalidated and could each be near UINT32_MAX.
    [[nodiscard]] std::uint64_t total_size() const noexcept {
        return static_cast<std::uint64_t>(kHeaderSize) + key_size + value_size;
    }
};

struct Record {
    std::uint64_t timestamp = 0;
    bool is_tombstone = false;
    std::string key;
    std::string value;
    std::uint32_t total_size = 0;  // What to add to this record's offset for the next one.
};

// zlib crc32 over `bytes`. Exposed because the tests and the recovery path both
// want it without going through a full encode.
[[nodiscard]] std::uint32_t crc32_of(std::span<const std::uint8_t> bytes);

// A tombstone ignores `value` and writes value_size = 0.
[[nodiscard]] std::vector<std::uint8_t> encode(std::string_view key,
                                               std::string_view value,
                                               std::uint64_t timestamp,
                                               bool is_tombstone);

// Header only -- lets a reader learn total_size before committing to a read of
// the whole record. Does not and cannot check the crc: the crc covers bytes
// this call hasn't seen.
[[nodiscard]] Result<Header> decode_header(std::span<const std::uint8_t> bytes);

// Full record, crc verified. `bytes` may be longer than the record; anything
// past total_size is ignored. Corruption if short, mis-shaped, or crc-mismatched.
[[nodiscard]] Result<Record> decode(std::span<const std::uint8_t> bytes);

}  // namespace kvstore::record
