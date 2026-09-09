#include "kvstore/bloom.hpp"

#include <cmath>
#include <utility>

#include "encoding.hpp"
#include "hash.hpp"
#include "kvstore/record.hpp"

namespace kvstore {

namespace {

// The two probe parameters, derived from one 64-bit hash.
//
// This is Kirsch-Mitzenmacher double hashing: k independent-looking probes at
// positions h, h+delta, h+2*delta, ... from a single hash, with a false-positive
// rate indistinguishable from k genuinely independent hashes. Seven probes
// therefore cost one pass over the key rather than seven.
//
// delta must never be zero, or every probe lands on the same bit and the filter
// degenerates to a one-bit test that says "maybe" to almost everything. The
// fallback is the golden-ratio constant, chosen only because it is odd and has a
// well-mixed bit pattern.
struct Probe {
    std::uint32_t position;
    std::uint32_t delta;
};

Probe probe_for(std::string_view key) noexcept {
    const std::uint64_t hash = hashing::hash64(key);
    std::uint32_t delta = static_cast<std::uint32_t>(hash >> 32);
    if (delta == 0) {
        delta = 0x9E3779B9u;
    }
    return Probe{static_cast<std::uint32_t>(hash), delta};
}

Probe probe_for_hash(std::uint64_t hash) noexcept {
    std::uint32_t delta = static_cast<std::uint32_t>(hash >> 32);
    if (delta == 0) {
        delta = 0x9E3779B9u;
    }
    return Probe{static_cast<std::uint32_t>(hash), delta};
}

}  // namespace

namespace bloom {

std::uint8_t probes_for(std::uint32_t bits_per_key) noexcept {
    // k = (m/n) * ln2 is the minimum of the false-positive curve: too few probes
    // and a lookup checks too little evidence, too many and the array saturates.
    const double ideal = static_cast<double>(bits_per_key) * 0.69314718055994531;
    long probes = std::lround(ideal);
    if (probes < 1) {
        probes = 1;
    }
    if (probes > 30) {
        probes = 30;
    }
    return static_cast<std::uint8_t>(probes);
}

}  // namespace bloom

BloomBuilder::BloomBuilder(std::uint32_t bits_per_key)
    : bits_per_key_(bits_per_key == 0 ? bloom::kDefaultBitsPerKey : bits_per_key) {}

void BloomBuilder::add(std::string_view key) { hashes_.push_back(hashing::hash64(key)); }

std::vector<std::uint8_t> BloomBuilder::finish() const {
    std::uint64_t bits = static_cast<std::uint64_t>(hashes_.size()) * bits_per_key_;
    if (bits < bloom::kMinBits) {
        bits = bloom::kMinBits;
    }
    if (bits > bloom::kMaxBits) {
        bits = bloom::kMaxBits;
    }
    // Whole bytes, so the array's length and its bit count agree exactly -- which
    // is what decode() checks to reject a truncated block.
    const std::uint64_t byte_count = (bits + 7) / 8;
    bits = byte_count * 8;

    const std::uint8_t probes = bloom::probes_for(bits_per_key_);
    const std::uint32_t modulus = static_cast<std::uint32_t>(bits);

    std::vector<std::uint8_t> out(bloom::kHeaderSize + static_cast<std::size_t>(byte_count), 0);
    encoding::put_u32(out, 0, modulus);
    out[4] = probes;

    for (const std::uint64_t hash : hashes_) {
        Probe probe = probe_for_hash(hash);
        for (std::uint8_t i = 0; i < probes; ++i) {
            const std::uint32_t position = probe.position % modulus;
            out[bloom::kHeaderSize + position / 8] |=
                static_cast<std::uint8_t>(1u << (position % 8));
            probe.position += probe.delta;
        }
    }

    const std::uint32_t crc = record::crc32_of(out);
    const std::size_t at = out.size();
    out.resize(at + bloom::kTrailerSize);
    encoding::put_u32(out, at, crc);
    return out;
}

Result<BloomFilter> BloomFilter::decode(std::span<const std::uint8_t> block) {
    if (block.size() < bloom::kHeaderSize + bloom::kTrailerSize) {
        return Status::corruption("bloom filter block is too small to be one");
    }

    const std::size_t payload_size = block.size() - bloom::kTrailerSize;
    if (encoding::get_u32(block, payload_size) !=
        record::crc32_of(block.subspan(0, payload_size))) {
        return Status::corruption("bloom filter block fails its crc");
    }

    BloomFilter filter;
    filter.num_bits_ = encoding::get_u32(block, 0);
    filter.num_probes_ = block[4];

    if (filter.num_bits_ == 0 || filter.num_probes_ == 0) {
        return Status::corruption("bloom filter declares no bits or no probes");
    }
    // The declared bit count must match the bytes actually present. Without this,
    // a truncated block would send maybe_contains() indexing past the array.
    const std::size_t bit_bytes = payload_size - bloom::kHeaderSize;
    if ((static_cast<std::uint64_t>(filter.num_bits_) + 7) / 8 != bit_bytes) {
        return Status::corruption("bloom filter bit count disagrees with its block size");
    }

    filter.bits_.assign(block.begin() + static_cast<std::ptrdiff_t>(bloom::kHeaderSize),
                        block.begin() + static_cast<std::ptrdiff_t>(payload_size));
    return filter;
}

bool BloomFilter::maybe_contains(std::string_view key) const noexcept {
    Probe probe = probe_for(key);
    for (std::uint8_t i = 0; i < num_probes_; ++i) {
        const std::uint32_t position = probe.position % num_bits_;
        // One clear bit is proof: the key was never added, because adding it
        // would have set this bit. That is the no-false-negatives guarantee, and
        // it is the whole reason a "no" here can be trusted without a disk read.
        if ((bits_[position / 8] & static_cast<std::uint8_t>(1u << (position % 8))) == 0) {
            return false;
        }
        probe.position += probe.delta;
    }
    return true;
}

}  // namespace kvstore
