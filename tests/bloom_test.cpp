#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kvstore/bloom.hpp"
#include "kvstore/record.hpp"
#include "kvstore_contract.hpp"

namespace kvstore {
namespace {

std::string key_at(int i) { return "key" + std::to_string(i); }

BloomFilter build(const std::vector<std::string>& keys,
                  std::uint32_t bits_per_key = bloom::kDefaultBitsPerKey) {
    BloomBuilder builder{bits_per_key};
    for (const std::string& key : keys) {
        builder.add(key);
    }
    auto filter = BloomFilter::decode(builder.finish());
    EXPECT_TRUE(filter.is_ok()) << filter.status().to_string();
    return filter.take();
}

// --- Probe arithmetic ------------------------------------------------------

TEST(BloomTest, ProbeCountFollowsTheOptimalFormula) {
    // k = bits_per_key * ln2, rounded.
    EXPECT_EQ(bloom::probes_for(10), 7);  // 6.93
    EXPECT_EQ(bloom::probes_for(20), 14);
    EXPECT_EQ(bloom::probes_for(4), 3);   // 2.77
}

TEST(BloomTest, ProbeCountIsClampedAtBothEnds) {
    // Zero probes would match every key, which is worse than useless.
    EXPECT_EQ(bloom::probes_for(0), 1);
    EXPECT_EQ(bloom::probes_for(1), 1);
    EXPECT_LE(bloom::probes_for(1000), 30);
}

// --- The guarantee ---------------------------------------------------------

// The property everything else depends on. A filter that ever said "absent"
// about a key that is present would silently lose data, so this is checked at a
// scale where a one-in-a-million bug would show up.
TEST(BloomTest, HasNoFalseNegativesOverManyKeys) {
    constexpr int kCount = 100000;
    std::vector<std::string> keys;
    keys.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        keys.push_back(key_at(i));
    }

    const BloomFilter filter = build(keys);
    for (const std::string& key : keys) {
        ASSERT_TRUE(filter.maybe_contains(key)) << "false negative for " << key;
    }
}

TEST(BloomTest, FalsePositiveRateIsNearOnePercentAtTenBits) {
    constexpr int kCount = 100000;
    std::vector<std::string> keys;
    keys.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        keys.push_back(key_at(i));
    }
    const BloomFilter filter = build(keys, 10);

    int false_positives = 0;
    for (int i = kCount; i < kCount * 2; ++i) {
        if (filter.maybe_contains(key_at(i))) {
            ++false_positives;
        }
    }

    const double rate = static_cast<double>(false_positives) / kCount;
    // The theoretical rate at 10 bits / 7 probes is ~0.82%. The bound is loose
    // because this asserts the hash spreads keys sensibly, not a precise figure.
    EXPECT_LT(rate, 0.03) << false_positives << " false positives in " << kCount;
}

TEST(BloomTest, MoreBitsPerKeyMeansFewerFalsePositives) {
    constexpr int kCount = 20000;
    std::vector<std::string> keys;
    for (int i = 0; i < kCount; ++i) {
        keys.push_back(key_at(i));
    }

    const BloomFilter thin = build(keys, 4);
    const BloomFilter fat = build(keys, 16);

    int thin_hits = 0;
    int fat_hits = 0;
    for (int i = kCount; i < kCount * 2; ++i) {
        const std::string absent = key_at(i);
        thin_hits += thin.maybe_contains(absent) ? 1 : 0;
        fat_hits += fat.maybe_contains(absent) ? 1 : 0;
    }
    EXPECT_GT(thin_hits, fat_hits) << "spending more bits must buy accuracy";
}

// --- Edges -----------------------------------------------------------------

// A table with no keys contains nothing, and the filter should say so rather
// than being a special case the caller has to remember.
TEST(BloomTest, AnEmptyFilterRejectsEverything) {
    const BloomFilter filter = build({});
    EXPECT_FALSE(filter.maybe_contains("anything"));
    EXPECT_FALSE(filter.maybe_contains(""));
}

TEST(BloomTest, ASingleKeyFilterFindsItsKeyAndLittleElse) {
    const BloomFilter filter = build({"alpha"});
    EXPECT_TRUE(filter.maybe_contains("alpha"));

    int hits = 0;
    for (int i = 0; i < 1000; ++i) {
        hits += filter.maybe_contains(key_at(i)) ? 1 : 0;
    }
    EXPECT_LT(hits, 100) << "a one-key filter should not match a tenth of everything";
}

TEST(BloomTest, KeysAreBinarySafe) {
    const std::string binary("k\0ey", 4);
    const std::string other("k\0ez", 4);
    const BloomFilter filter = build({binary});

    EXPECT_TRUE(filter.maybe_contains(binary));
    // Not guaranteed false in general, but with one key in 64 bits it should be.
    EXPECT_FALSE(filter.maybe_contains(other));
}

TEST(BloomTest, ATinyFilterStillGetsTheMinimumBitBudget) {
    BloomBuilder builder{10};
    builder.add("only");
    const BloomFilter filter = BloomFilter::decode(builder.finish()).take();
    EXPECT_GE(filter.num_bits(), bloom::kMinBits);
    EXPECT_EQ(filter.num_probes(), 7);
}

// --- The encoded block -----------------------------------------------------

TEST(BloomTest, TheBlockRoundTripsThroughItsEncoding) {
    BloomBuilder builder{10};
    builder.add("alpha");
    builder.add("bravo");
    EXPECT_EQ(builder.key_count(), 2u);

    const std::vector<std::uint8_t> block = builder.finish();
    auto filter = BloomFilter::decode(block);
    ASSERT_TRUE(filter.is_ok()) << filter.status().to_string();
    EXPECT_TRUE(filter->maybe_contains("alpha"));
    EXPECT_TRUE(filter->maybe_contains("bravo"));
}

TEST(BloomTest, AFlippedBitInTheBlockFailsItsCrc) {
    BloomBuilder builder{10};
    builder.add("alpha");
    std::vector<std::uint8_t> block = builder.finish();

    block[bloom::kHeaderSize] ^= 0xFF;
    EXPECT_STATUS(StatusCode::Corruption, BloomFilter::decode(block).status());
}

TEST(BloomTest, ABlockTooSmallToBeAFilterIsRejected) {
    const std::vector<std::uint8_t> block(bloom::kHeaderSize, 0);
    EXPECT_STATUS(StatusCode::Corruption, BloomFilter::decode(block).status());
}

// A declared bit count larger than the bytes present would send maybe_contains
// indexing past the end of the array -- so it is rejected at decode, not trusted.
TEST(BloomTest, ABitCountThatDisagreesWithTheBlockSizeIsRejected) {
    BloomBuilder builder{10};
    builder.add("alpha");
    std::vector<std::uint8_t> block = builder.finish();

    // Rewrite num_bits to something far too large, then repair the crc so that
    // the size check is what fires rather than the checksum.
    const std::size_t payload = block.size() - bloom::kTrailerSize;
    block[0] = 0xFF;
    block[1] = 0xFF;
    const std::uint32_t crc =
        record::crc32_of(std::span<const std::uint8_t>{block.data(), payload});
    block[payload + 0] = static_cast<std::uint8_t>(crc);
    block[payload + 1] = static_cast<std::uint8_t>(crc >> 8);
    block[payload + 2] = static_cast<std::uint8_t>(crc >> 16);
    block[payload + 3] = static_cast<std::uint8_t>(crc >> 24);

    EXPECT_STATUS(StatusCode::Corruption, BloomFilter::decode(block).status());
}

}  // namespace
}  // namespace kvstore
