#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kvstore::encoding {

// Little-endian integer read/write, written a byte at a time rather than by
// memcpy'ing an integer.
//
// This is what makes the on-disk formats little-endian on *every* host
// regardless of the CPU's own byte order -- a file written on one machine has
// to be readable on another. memcpy would silently write big-endian bytes on a
// big-endian machine and produce a file only that machine can read.
//
// Shared by the record format and the hint format, which is deliberate: two
// formats in one engine should not disagree about how an integer looks.

inline void put_u32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    out[at + 0] = static_cast<std::uint8_t>(v);
    out[at + 1] = static_cast<std::uint8_t>(v >> 8);
    out[at + 2] = static_cast<std::uint8_t>(v >> 16);
    out[at + 3] = static_cast<std::uint8_t>(v >> 24);
}

inline void put_u64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t at) {
    return static_cast<std::uint32_t>(in[at + 0]) |
           (static_cast<std::uint32_t>(in[at + 1]) << 8) |
           (static_cast<std::uint32_t>(in[at + 2]) << 16) |
           (static_cast<std::uint32_t>(in[at + 3]) << 24);
}

inline std::uint64_t get_u64(std::span<const std::uint8_t> in, std::size_t at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(in[at + i]) << (8 * i);
    }
    return v;
}

}  // namespace kvstore::encoding
