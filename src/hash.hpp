#pragma once

#include <cstdint>
#include <string_view>

namespace kvstore {

// A 64-bit FNV-1a hash, for bloom filter probes and nothing else.
//
// **Why not reuse zlib's crc32, which is already linked.** A crc32 is an
// error-detecting code, not a hash. It is linear over GF(2) -- flipping a bit in
// the input flips a predictable set of bits in the output -- which is exactly the
// property that makes it good at catching burst errors and bad at spreading keys
// uniformly across a bit array. It is also only 32 bits, and the double-hashing
// scheme below wants 64 so that one pass over the key yields both probe
// parameters.
//
// There is a second, quieter reason: a table's blocks are already checksummed
// with crc32, and deriving the filter bits from the same function would correlate
// the two. Nothing would break, but "the filter agrees with the checksum" is not
// a property anyone should have to reason about.
//
// FNV-1a rather than something stronger because the requirement is uniformity on
// arbitrary byte strings, not resistance to an adversary choosing keys. It is
// eight lines, has no lookup table, and its avalanche behaviour is well
// understood. If keys ever come from somewhere hostile, this is the one function
// to replace.
namespace hashing {

inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] inline std::uint64_t hash64(std::string_view data) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const char c : data) {
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace hashing
}  // namespace kvstore
