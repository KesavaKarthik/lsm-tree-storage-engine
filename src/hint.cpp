#include "kvstore/hint.hpp"

#include <cstring>

#include "encoding.hpp"
#include "kvstore/record.hpp"

namespace kvstore::hint {
namespace {

using encoding::get_u32;
using encoding::get_u64;
using encoding::put_u32;
using encoding::put_u64;

constexpr std::size_t kCrcSize = 4;

}  // namespace

std::vector<std::uint8_t> encode_entry(std::string_view key,
                                       std::uint64_t timestamp,
                                       std::uint64_t record_offset,
                                       std::uint32_t record_size) {
    const auto key_size = static_cast<std::uint32_t>(key.size());
    std::vector<std::uint8_t> out(kEntryHeaderSize + key_size);

    std::size_t at = kCrcSize;  // The crc goes in last -- it covers what follows.
    put_u64(out, at, timestamp);
    at += 8;
    put_u64(out, at, record_offset);
    at += 8;
    put_u32(out, at, record_size);
    at += 4;
    put_u32(out, at, key_size);
    at += 4;

    if (key_size > 0) {
        std::memcpy(out.data() + at, key.data(), key_size);
    }

    put_u32(out, 0, record::crc32_of(std::span<const std::uint8_t>{out}.subspan(kCrcSize)));
    return out;
}

Result<Entry> decode_entry(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kEntryHeaderSize) {
        return Status::corruption("hint entry header truncated");
    }

    const std::uint32_t crc = get_u32(bytes, 0);
    Entry entry;
    entry.timestamp = get_u64(bytes, 4);
    entry.record_offset = get_u64(bytes, 12);
    entry.record_size = get_u32(bytes, 20);
    const std::uint32_t key_size = get_u32(bytes, 24);

    // Same rule as the record format, and for the same reason: a zero-length
    // key is the signature of a corrupt header, not something a writer produces.
    if (key_size == 0) {
        return Status::corruption("hint entry declares a zero-length key");
    }
    // A record cannot be smaller than a record header, and cannot be larger
    // than the engine carries a size in. Checking here means a hostile length
    // never reaches the read it would have sized.
    if (entry.record_size < record::kHeaderSize) {
        return Status::corruption("hint entry declares an impossibly small record");
    }

    const std::uint64_t total = static_cast<std::uint64_t>(kEntryHeaderSize) + key_size;
    if (bytes.size() < total) {
        return Status::corruption("hint entry key truncated");
    }

    const std::uint32_t expected =
        record::crc32_of(bytes.subspan(kCrcSize, static_cast<std::size_t>(total) - kCrcSize));
    if (expected != crc) {
        return Status::corruption("hint entry crc mismatch");
    }

    entry.key.assign(reinterpret_cast<const char*>(bytes.data()) + kEntryHeaderSize, key_size);
    return entry;
}

std::vector<std::uint8_t> encode_footer(std::uint64_t data_file_size,
                                        std::uint64_t entry_count) {
    std::vector<std::uint8_t> out(kFooterSize);
    std::memcpy(out.data(), kMagic, sizeof(kMagic));
    put_u64(out, 4, data_file_size);
    put_u64(out, 12, entry_count);
    // Covers the magic and both counts -- everything before itself. Unlike a
    // record's crc this one sits at the *end*, because the footer's job is to
    // prove the file is complete, and a marker that proves completeness has to
    // be the last thing written.
    put_u32(out, 20, record::crc32_of(std::span<const std::uint8_t>{out}.subspan(0, 20)));
    return out;
}

Result<Footer> decode_footer(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kFooterSize) {
        return Status::corruption("hint footer is the wrong size");
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return Status::corruption("hint footer magic missing");
    }

    Footer footer;
    footer.data_file_size = get_u64(bytes, 4);
    footer.entry_count = get_u64(bytes, 12);

    const std::uint32_t expected = record::crc32_of(bytes.subspan(0, 20));
    if (expected != get_u32(bytes, 20)) {
        return Status::corruption("hint footer crc mismatch");
    }
    return footer;
}

}  // namespace kvstore::hint
