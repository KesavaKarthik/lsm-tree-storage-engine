#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "kvstore/record.hpp"
#include "raw_record.hpp"

namespace kvstore::record {
namespace {

using testing_support::make_raw_header;

TEST(RecordTest, RoundTripsKeyAndValue) {
    const auto bytes = encode("alpha", "one", 12345, false);

    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->key, "alpha");
    EXPECT_EQ(decoded->value, "one");
    EXPECT_EQ(decoded->timestamp, 12345u);
    EXPECT_FALSE(decoded->is_tombstone);
    EXPECT_EQ(decoded->total_size, kHeaderSize + 5 + 3);
}

TEST(RecordTest, HeaderIsFixedSize) {
    // The layout is a promise to every file already on disk. If this fails,
    // the format changed and old databases are unreadable.
    EXPECT_EQ(kHeaderSize, 21u);
    EXPECT_EQ(encode("k", "v", 0, false).size(), kHeaderSize + 2);
}

TEST(RecordTest, TombstoneCarriesNoValue) {
    const auto bytes = encode("alpha", "ignored", 7, true);
    EXPECT_EQ(bytes.size(), kHeaderSize + 5);  // No value bytes at all.

    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_TRUE(decoded->is_tombstone);
    EXPECT_EQ(decoded->key, "alpha");
    EXPECT_TRUE(decoded->value.empty());
}

TEST(RecordTest, EncodingIsLittleEndianRegardlessOfHost) {
    // key_size lives at byte 13. A one-byte key must read 01 00 00 00 on every
    // machine, or a file written here won't open there.
    const auto bytes = encode("k", "", 0, false);
    EXPECT_EQ(bytes[13], 0x01);
    EXPECT_EQ(bytes[14], 0x00);
    EXPECT_EQ(bytes[15], 0x00);
    EXPECT_EQ(bytes[16], 0x00);
}

TEST(RecordTest, BinarySafeKeysAndValues) {
    const std::string key("a\0b", 3);
    const std::string value("x\0\0y", 4);

    auto decoded = decode(encode(key, value, 1, false));
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->key, key);
    EXPECT_EQ(decoded->value, value);
}

TEST(RecordTest, CrcCatchesAFlippedValueByte) {
    auto bytes = encode("alpha", "one", 1, false);
    bytes.back() ^= 0xFF;  // Damage the last byte of the value.

    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption()) << decoded.status().to_string();
}

TEST(RecordTest, CrcCoversTheHeaderToo) {
    // The point of putting the crc first and covering everything after it: a
    // corrupted *header* is caught as well, not just a corrupted payload.
    auto bytes = encode("alpha", "one", 1, false);
    bytes[4] ^= 0xFF;  // First byte of the timestamp.

    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption());
}

TEST(RecordTest, TruncatedRecordIsCorruption) {
    auto bytes = encode("alpha", "one", 1, false);
    bytes.resize(bytes.size() - 1);  // What a mid-write crash leaves behind.

    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption());
}

TEST(RecordTest, TruncatedHeaderIsCorruption) {
    const auto bytes = encode("alpha", "one", 1, false);
    const std::vector<std::uint8_t> partial(bytes.begin(), bytes.begin() + 10);

    auto decoded = decode(partial);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption());
}

TEST(RecordTest, ZeroLengthKeyHeaderIsRejected) {
    // Caught before the crc, because a garbage header must not be trusted far
    // enough to compute a length from it.
    std::vector<std::uint8_t> bytes(kHeaderSize, 0);
    auto header = decode_header(bytes);
    EXPECT_FALSE(header.is_ok());
    EXPECT_TRUE(header.status().is_corruption());
}

// --- Hostile size fields ---------------------------------------------------
//
// These pin down the load-bearing bounds checks. Fuzzing (41k inputs, Aug 2026)
// established the code is clean here, but that finding lived only in NOTES.md --
// nothing in the suite would have caught someone later moving a length check to
// after the read it guards. Under ASan these are also memory-safety tests: every
// buffer is exactly sized, so an overread has nowhere harmless to land.

TEST(RecordTest, ImpossiblyLargeTotalIsRejected) {
    // 21 + 0xFFFFFFFF + 0xFFFFFFFF is ~8.6GB. The two size fields are 32-bit
    // each, so their sum overflows the uint32_t the index and read_at use --
    // which is why kMaxRecordSize exists and is checked here rather than left to
    // a narrowing cast further downstream.
    auto decoded = decode_header(make_raw_header(0xFFFFFFFFu, 0xFFFFFFFFu));
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption()) << decoded.status().to_string();
}

TEST(RecordTest, TotalExactlyAtTheLimitIsAcceptedByHeaderDecode) {
    // The boundary on the legal side, so the check can't quietly become
    // off-by-one strict. Header shape only -- no buffer this large is allocated.
    const std::uint32_t key_size = static_cast<std::uint32_t>(kMaxRecordSize - kHeaderSize);
    auto decoded = decode_header(make_raw_header(key_size, 0));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->total_size(), kMaxRecordSize);
}

TEST(RecordTest, HostileKeySizeDoesNotReadPastTheBuffer) {
    // Header alone, claiming a ~4GB key. Must be rejected on the length, before
    // any attempt to copy the key out.
    auto decoded = decode(make_raw_header(0xFFFFFF00u, 0));
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption());
}

TEST(RecordTest, HostileValueSizeDoesNotReadPastTheBuffer) {
    // Honest key, absurd value length -- the key copy succeeds and the value
    // copy is the one that would run off the end.
    auto bytes = make_raw_header(5, 0xFFFFFF00u);
    const std::string key = "alpha";
    bytes.insert(bytes.end(), key.begin(), key.end());

    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption());
}

TEST(RecordTest, ValueSizeOneBytePastTheEndIsRejected) {
    // The off-by-one, which is the version that actually happens: a real record
    // whose value_size says 4 when 3 bytes follow.
    auto bytes = encode("alpha", "one", 1, false);
    bytes[17] = 4;

    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_corruption());
}

TEST(RecordTest, DecodeIgnoresTrailingBytes) {
    // A reader hands decode() a buffer that may contain the next record too.
    auto bytes = encode("alpha", "one", 1, false);
    const auto expected_size = bytes.size();
    bytes.push_back(0xAB);
    bytes.push_back(0xCD);

    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->total_size, expected_size);
    EXPECT_EQ(decoded->value, "one");
}

}  // namespace
}  // namespace kvstore::record
