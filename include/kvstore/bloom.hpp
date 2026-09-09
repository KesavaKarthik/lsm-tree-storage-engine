#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// A bloom filter: a compact, probabilistic "is this key definitely absent?".
//
// **The asymmetry is the whole point.** A bloom filter never says "absent" about
// a key that is present -- there are no false negatives, ever -- but it sometimes
// says "maybe present" about a key that is not. So a "no" is a *final answer*,
// arrived at entirely in RAM, and a "maybe" costs one wasted block read. That
// trade is what makes it worth carrying: an LSM read otherwise has to interrogate
// every table in the tree before it can conclude a key does not exist, and the
// engine's own statistics already measure that at one table probe per table.
//
// The cost is honest and worth stating plainly: at ten bits per key a filter is
// about 1.25 bytes per key, held resident for every open table. That is roughly
// twenty times better than Bitcask's ~30-bytes-per-key index, but it is still
// *proportional to the number of keys* -- so "only a sparse index stays in RAM"
// acquires a footnote here. Per-block filters paged in on demand are the fully
// general answer; they are deliberately not built, because at this scale the
// complexity would buy nothing measurable.
namespace bloom {

// Ten bits per key gives roughly a 1% false-positive rate, which is the knee of
// the curve: going to twenty bits costs twice the memory to reach ~0.05%, and
// dropping to five costs ~10% for half the memory. Most of the benefit is bought
// by the first ten bits.
inline constexpr std::uint32_t kDefaultBitsPerKey = 10;

//   filter block = [ num_bits 4B | num_probes 1B | bits... | crc32 4B ]
//
// num_probes is stored rather than recomputed so a reader never has to agree
// with a writer about a rounding rule -- the file says how it was built.
inline constexpr std::size_t kHeaderSize = 5;
inline constexpr std::size_t kTrailerSize = 4;

// A floor, so that a table with three keys in it does not get a nine-bit filter
// whose false-positive rate is essentially 100%.
inline constexpr std::uint32_t kMinBits = 64;

// Bit positions are computed modulo num_bits as a uint32, so the array cannot be
// larger than that. Rounded down to a whole byte.
inline constexpr std::uint32_t kMaxBits = 0xFFFFFFF8u;

// The probe count that minimises false positives for a given bits-per-key:
// k = (m/n) * ln2. Clamped to [1, 30] -- zero probes would match everything, and
// past thirty the extra hashing costs more than the accuracy is worth.
[[nodiscard]] std::uint8_t probes_for(std::uint32_t bits_per_key) noexcept;

}  // namespace bloom

// Accumulates keys and renders a filter block.
//
// It buffers **hashes, not bits** -- eight bytes per key -- because the size of
// the bit array depends on how many keys there turn out to be, and that is not
// known until finish(). Hashing once up front also means the keys themselves need
// not be kept alive.
class BloomBuilder {
public:
    explicit BloomBuilder(std::uint32_t bits_per_key = bloom::kDefaultBitsPerKey);

    // Every key that goes into the table must go in here too -- **including the
    // keys of tombstones.** A tombstone is an answer, not an absence: if the
    // filter denies a deleted key, the lookup skips this table and finds the old
    // value in an older one, and the delete silently undoes itself.
    void add(std::string_view key);

    [[nodiscard]] std::size_t key_count() const noexcept { return hashes_.size(); }

    // The encoded filter block, crc included. Safe to call on an empty builder:
    // the result is a small all-zero filter that answers "absent" to everything,
    // which is exactly right for a table with no keys in it.
    [[nodiscard]] std::vector<std::uint8_t> finish() const;

private:
    std::uint32_t bits_per_key_;
    std::vector<std::uint64_t> hashes_;
};

// Reads a filter block and answers membership questions from RAM.
class BloomFilter {
public:
    [[nodiscard]] static Result<BloomFilter> decode(std::span<const std::uint8_t> block);

    // false means *definitely not in the table* -- a final answer.
    // true means *maybe*, and the caller still has to look.
    [[nodiscard]] bool maybe_contains(std::string_view key) const noexcept;

    [[nodiscard]] std::uint32_t num_bits() const noexcept { return num_bits_; }
    [[nodiscard]] std::uint8_t num_probes() const noexcept { return num_probes_; }

private:
    BloomFilter() = default;

    std::vector<std::uint8_t> bits_;
    std::uint32_t num_bits_ = 0;
    std::uint8_t num_probes_ = 0;
};

}  // namespace kvstore
