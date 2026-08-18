#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kvstore::wire {

// Big-endian integer read/write, a byte at a time, for the network protocol.
//
// Deliberately a second set of helpers rather than a reuse of src/encoding.hpp,
// which does the identical job in *little*-endian for the on-disk record and
// hint formats. Two functions with the same name and shape that disagree about
// byte order is a bug that hides perfectly: on x86 both are just as fast, both
// round-trip through themselves, and every unit test passes. It only surfaces
// when a real peer or a real packet capture reads the number backwards.
// Separate namespaces mean the wrong one cannot be reached for by accident.
//
// Why the two formats differ at all:
//
//   Disk is little-endian because it matches every CPU this will ever run on,
//   so a hexdump of a record reads the same way the debugger shows the integer.
//   Nothing but this engine ever parses those bytes.
//
//   The wire is big-endian because that is network byte order, which htonl()
//   exists to produce and which every protocol dissector -- Wireshark, tcpdump,
//   anything anyone would debug this with -- assumes without being told. The
//   cost of being clever here is paid by whoever opens a packet capture.
//
// Written a byte at a time rather than memcpy'd, for the same reason
// encoding.hpp is: memcpy would emit the host's own byte order and produce a
// stream only a machine of the same endianness could read.

inline void put_u32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    out[at + 0] = static_cast<std::uint8_t>(v >> 24);
    out[at + 1] = static_cast<std::uint8_t>(v >> 16);
    out[at + 2] = static_cast<std::uint8_t>(v >> 8);
    out[at + 3] = static_cast<std::uint8_t>(v);
}

inline void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t at) {
    return (static_cast<std::uint32_t>(in[at + 0]) << 24) |
           (static_cast<std::uint32_t>(in[at + 1]) << 16) |
           (static_cast<std::uint32_t>(in[at + 2]) << 8) |
           static_cast<std::uint32_t>(in[at + 3]);
}

}  // namespace kvstore::wire
