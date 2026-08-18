#include "kvstore/record.hpp"

#include <zlib.h>

#include <cstring>

#include "encoding.hpp"

namespace kvstore::record {
namespace {

// Little-endian integer access, shared with the hint format. See encoding.hpp
// for why these are byte-at-a-time rather than a memcpy.
using encoding::get_u32;
using encoding::get_u64;
using encoding::put_u32;
using encoding::put_u64;

}  // namespace

std::uint32_t crc32_of(std::span<const std::uint8_t> bytes) {
    uLong crc = ::crc32(0L, Z_NULL, 0);
    crc = ::crc32(crc, bytes.data(), static_cast<uInt>(bytes.size()));
    return static_cast<std::uint32_t>(crc);
}

std::vector<std::uint8_t> encode(std::string_view key,
                                 std::string_view value,
                                 std::uint64_t timestamp,
                                 bool is_tombstone) {
    const auto key_size = static_cast<std::uint32_t>(key.size());
    const auto value_size = is_tombstone ? 0u : static_cast<std::uint32_t>(value.size());

    std::vector<std::uint8_t> out(kHeaderSize + key_size + value_size);

    std::size_t at = kCrcSize;  // The crc goes in last -- it covers what follows.
    put_u64(out, at, timestamp);
    at += 8;
    out[at++] = is_tombstone ? kFlagTombstone : std::uint8_t{0};
    put_u32(out, at, key_size);
    at += 4;
    put_u32(out, at, value_size);
    at += 4;

    if (key_size > 0) {
        std::memcpy(out.data() + at, key.data(), key_size);
        at += key_size;
    }
    if (value_size > 0) {
        std::memcpy(out.data() + at, value.data(), value_size);
    }

    put_u32(out, 0, crc32_of(std::span<const std::uint8_t>{out}.subspan(kCrcSize)));
    return out;
}

Result<Header> decode_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        return Status::corruption("record header truncated");
    }

    Header h;
    h.crc = get_u32(bytes, 0);
    h.timestamp = get_u64(bytes, 4);
    h.flags = bytes[12];
    h.key_size = get_u32(bytes, 13);
    h.value_size = get_u32(bytes, 17);

    // Shape checks that don't need the crc. Cheap, and they stop a garbage
    // header from sending the caller off to read a nonsense length.
    if (h.key_size == 0) {
        return Status::corruption("record header declares a zero-length key");
    }
    if (h.is_tombstone() && h.value_size != 0) {
        return Status::corruption("tombstone record declares a non-zero value size");
    }
    // The two size fields are 32-bit each, so their sum can exceed what the rest
    // of the engine carries a size in. Rejecting here means every caller gets a
    // total it can narrow to uint32_t safely.
    if (h.total_size() > kMaxRecordSize) {
        return Status::corruption("record header declares a size larger than a record may be");
    }
    return h;
}

Result<Record> decode(std::span<const std::uint8_t> bytes) {
    auto header = decode_header(bytes);
    if (!header.is_ok()) {
        return header.status();
    }

    const std::uint64_t total = header->total_size();
    if (bytes.size() < total) {
        return Status::corruption("record body truncated");
    }

    // Everything after the crc field, header included.
    const std::uint32_t expected =
        crc32_of(bytes.subspan(kCrcSize, static_cast<std::size_t>(total) - kCrcSize));
    if (expected != header->crc) {
        return Status::corruption("record crc mismatch");
    }

    const auto* base = reinterpret_cast<const char*>(bytes.data());

    Record r;
    r.timestamp = header->timestamp;
    r.is_tombstone = header->is_tombstone();
    r.key.assign(base + kHeaderSize, header->key_size);
    r.value.assign(base + kHeaderSize + header->key_size, header->value_size);
    r.total_size = static_cast<std::uint32_t>(total);
    return r;
}

}  // namespace kvstore::record
