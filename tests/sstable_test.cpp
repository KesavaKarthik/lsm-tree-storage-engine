#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/sstable.hpp"
#include "kvstore_contract.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::flip_byte_at;
using testing_support::overwrite_at;
using testing_support::TempDir;
using testing_support::truncate_by;

// The footer, read straight off the end of a file.
sstable::Footer footer_of(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.is_open());
    in.seekg(-static_cast<std::streamoff>(sstable::kFooterSize), std::ios::end);
    std::vector<std::uint8_t> bytes(sstable::kFooterSize);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto footer = sstable::decode_footer(bytes);
    EXPECT_TRUE(footer.is_ok()) << footer.status().to_string();
    return footer.is_ok() ? *footer : sstable::Footer{};
}

// --- Helpers ---------------------------------------------------------------

// Three-digit keys so lexicographic order and numeric order agree, which keeps
// the expectations in these tests readable.
std::string key_at(int i) { return "key" + std::string(3 - std::to_string(i).size(), '0') +
                                   std::to_string(i); }

// Builds a table at `path` from `count` keys, small blocks by default so that a
// handful of keys spans several blocks -- the only way to tell whether the
// sparse index is doing anything.
void build_table(const std::filesystem::path& path, int count,
                 std::uint32_t block_size = 64) {
    auto created = SSTableBuilder::create(path, block_size);
    ASSERT_TRUE(created.is_ok()) << created.status().to_string();
    SSTableBuilder builder = created.take();

    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(builder.add(key_at(i), "value" + std::to_string(i), false).is_ok());
    }
    ASSERT_TRUE(builder.finish().is_ok());
}

std::unique_ptr<SSTable> open_table_or_fail(const std::filesystem::path& path,
                                            LsmStats* stats = nullptr) {
    auto table = SSTable::open(path, 1, stats);
    EXPECT_TRUE(table.is_ok()) << table.status().to_string();
    return table.is_ok() ? table.take() : nullptr;
}

// --- Footer ----------------------------------------------------------------

TEST(SSTableFormat, AFooterRoundTrips) {
    sstable::Footer footer;
    footer.index_offset = 4096;
    footer.index_size = 128;
    footer.filter_offset = 4000;
    footer.filter_size = 96;
    footer.entry_count = 42;

    const std::vector<std::uint8_t> bytes = sstable::encode_footer(footer);
    ASSERT_EQ(bytes.size(), sstable::kFooterSize);

    auto decoded = sstable::decode_footer(bytes);
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->index_offset, 4096u);
    EXPECT_EQ(decoded->index_size, 128u);
    EXPECT_EQ(decoded->filter_offset, 4000u);
    EXPECT_EQ(decoded->filter_size, 96u);
    EXPECT_EQ(decoded->entry_count, 42u);
    EXPECT_EQ(decoded->format_version, sstable::kFormatVersion);
}

TEST(SSTableFormat, TheMagicIsTheLastFourBytes) {
    const std::vector<std::uint8_t> bytes = sstable::encode_footer(sstable::Footer{});
    EXPECT_EQ(bytes[40], 'K');
    EXPECT_EQ(bytes[41], 'V');
    EXPECT_EQ(bytes[42], 'S');
    EXPECT_EQ(bytes[43], 'T');
}

TEST(SSTableFormat, AFooterWithoutTheMagicIsRejected) {
    std::vector<std::uint8_t> bytes = sstable::encode_footer(sstable::Footer{});
    bytes[43] ^= 0xFF;
    EXPECT_STATUS(StatusCode::Corruption, sstable::decode_footer(bytes).status());
}

TEST(SSTableFormat, AFooterWithAFlippedBitFailsItsCrc) {
    std::vector<std::uint8_t> bytes = sstable::encode_footer(sstable::Footer{});
    bytes[4] ^= 0xFF;  // Inside index_size, which the crc covers.
    EXPECT_STATUS(StatusCode::Corruption, sstable::decode_footer(bytes).status());
}

// The filter block sits between the data and the index, in the slot the footer
// reserved a phase before anything filled it.
TEST(SSTableFormat, TheFilterBlockSitsBetweenTheDataAndTheIndex) {
    TempDir dir;
    const auto path = dir.path() / "table.sst";
    build_table(path, 20);

    const sstable::Footer footer = footer_of(path);
    EXPECT_GT(footer.filter_size, 0u) << "a table written now carries a filter";
    EXPECT_LT(footer.filter_offset, footer.index_offset);
    EXPECT_EQ(footer.filter_offset + footer.filter_size, footer.index_offset)
        << "the three regions must be contiguous, in order";
}

// The compatibility claim the reserved slot was for, tested rather than
// asserted: a table whose footer says "no filter" -- which is exactly what every
// table written before filters existed says -- still opens and still answers
// correctly. It just consults no filter.
TEST(SSTableFormat, ATableWithNoFilterStillOpensAndAnswersCorrectly) {
    TempDir dir;
    const auto path = dir.path() / "table.sst";
    build_table(path, 50);

    sstable::Footer footer = footer_of(path);
    ASSERT_GT(footer.filter_size, 0u);
    footer.filter_size = 0;  // Pretend it was written before filters existed.
    overwrite_at(path, std::filesystem::file_size(path) - sstable::kFooterSize,
                 sstable::encode_footer(footer));

    LsmStats stats;
    auto table = open_table_or_fail(path, &stats);
    ASSERT_NE(table, nullptr);

    std::string value;
    for (const int i : {0, 25, 49}) {
        auto result = table->lookup(key_at(i), &value);
        ASSERT_TRUE(result.is_ok()) << result.status().to_string();
        EXPECT_EQ(*result, Lookup::Found);
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
    EXPECT_EQ(stats.bloom_checks.load(), 0u) << "there is no filter to consult";
}

TEST(SSTableCorruption, AFlippedByteInTheFilterBlockFailsItsCrc) {
    TempDir dir;
    const auto path = dir.path() / "table.sst";
    build_table(path, 50);

    const sstable::Footer footer = footer_of(path);
    ASSERT_GT(footer.filter_size, 0u);
    flip_byte_at(path, footer.filter_offset + 6);

    EXPECT_STATUS(StatusCode::Corruption, SSTable::open(path, 1).status());
}

// --- Building --------------------------------------------------------------

TEST(SSTableBuild, RejectsKeysThatArriveOutOfOrder) {
    TempDir dir;
    auto created = SSTableBuilder::create(dir.path() / "t.sst", 4096);
    ASSERT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();

    EXPECT_OK(builder.add("bravo", "2", false));
    // Not silently sorted: an unsorted source would build a file that passes
    // every write-side check and then fails lookups at random later.
    EXPECT_STATUS(StatusCode::InvalidArgument, builder.add("alpha", "1", false));
    EXPECT_STATUS(StatusCode::InvalidArgument, builder.add("bravo", "again", false));
    EXPECT_OK(builder.finish());
}

TEST(SSTableBuild, RejectsAnEmptyKey) {
    TempDir dir;
    auto created = SSTableBuilder::create(dir.path() / "t.sst", 4096);
    ASSERT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();
    EXPECT_STATUS(StatusCode::InvalidArgument, builder.add("", "value", false));
}

TEST(SSTableBuild, RejectsAddAfterFinishAndFinishTwice) {
    TempDir dir;
    auto created = SSTableBuilder::create(dir.path() / "t.sst", 4096);
    ASSERT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();

    EXPECT_OK(builder.add("alpha", "1", false));
    EXPECT_OK(builder.finish());
    EXPECT_STATUS(StatusCode::InvalidArgument, builder.add("bravo", "2", false));
    EXPECT_STATUS(StatusCode::InvalidArgument, builder.finish());
}

// A leftover temp from an interrupted flush must be replaced, not appended to --
// appending would produce a file with a stale footer buried in its data region.
TEST(SSTableBuild, ReplacesAStaleTempRatherThanAppendingToIt) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 50);
    const auto stale_size = std::filesystem::file_size(path);

    build_table(path, 3);
    EXPECT_LT(std::filesystem::file_size(path), stale_size);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->entry_count(), 3u);
}

TEST(SSTableBuild, AnEmptyTableIsWellFormed) {
    TempDir dir;
    const auto path = dir.path() / "empty.sst";
    auto created = SSTableBuilder::create(path, 4096);
    ASSERT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();
    EXPECT_OK(builder.finish());

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->entry_count(), 0u);
    EXPECT_EQ(table->block_count(), 0u);
    EXPECT_TRUE(table->min_key().empty());
    EXPECT_TRUE(table->max_key().empty());

    std::string value;
    auto result = table->lookup("anything", &value);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(*result, Lookup::NotPresent);
}

// --- Reading ---------------------------------------------------------------

TEST(SSTableRead, RoundTripsEveryKeyItWasGiven) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 200);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->entry_count(), 200u);

    for (int i = 0; i < 200; ++i) {
        std::string value;
        auto result = table->lookup(key_at(i), &value);
        ASSERT_TRUE(result.is_ok()) << result.status().to_string();
        EXPECT_EQ(*result, Lookup::Found) << "missing " << key_at(i);
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

// The sparse index earns its keep only if there is more than one block to
// choose between; with 64-byte blocks and 200 keys there are many.
TEST(SSTableRead, TheSparseIndexNamesManyBlocksAndFindsKeysInEachOfThem) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 200, /*block_size=*/64);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    ASSERT_GT(table->block_count(), 10u) << "the test needs many blocks to be meaningful";
    EXPECT_EQ(table->min_key(), key_at(0));
    EXPECT_EQ(table->max_key(), key_at(199));

    // First, last, and a key in the middle of the file.
    for (const int i : {0, 1, 99, 100, 198, 199}) {
        std::string value;
        auto result = table->lookup(key_at(i), &value);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(*result, Lookup::Found) << "missing " << key_at(i);
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

TEST(SSTableRead, IndexKeysAreTheLastKeyOfEachBlockAndAscend) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 200, 64);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    const auto& index = table->index();
    ASSERT_FALSE(index.empty());

    for (std::size_t i = 1; i < index.size(); ++i) {
        EXPECT_LT(index[i - 1].key, index[i].key);
    }
    // The last index entry is the largest key in the table, which is what makes
    // max_key() free.
    EXPECT_EQ(index.back().key, table->max_key());
}

TEST(SSTableRead, AnAbsentKeyIsNotPresentRatherThanAnError) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 50);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);

    std::string value;
    for (const char* absent : {"aaa", "key0005x", "zzz"}) {
        auto result = table->lookup(absent, &value);
        ASSERT_TRUE(result.is_ok()) << result.status().to_string();
        EXPECT_EQ(*result, Lookup::NotPresent) << absent;
    }
}

// Keys outside [min_key, max_key] are rejected without touching the disk. Step 2
// puts the bloom filter in exactly this spot, for the keys that fall inside the
// range but still are not there.
TEST(SSTableRead, AKeyOutsideTheTablesRangeCostsNoBlockRead) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 50);

    LsmStats stats;
    auto table = open_table_or_fail(path, &stats);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(stats.block_reads.load(), 0u) << "opening a table is not read amplification";

    std::string value;
    ASSERT_TRUE(table->lookup("aaa", &value).is_ok());
    ASSERT_TRUE(table->lookup("zzz", &value).is_ok());
    EXPECT_EQ(stats.block_reads.load(), 0u);

    // A key inside the range does cost exactly one.
    ASSERT_TRUE(table->lookup(key_at(25), &value).is_ok());
    EXPECT_EQ(stats.block_reads.load(), 1u);
}

TEST(SSTableRead, ATombstoneReadsBackAsDeletedRatherThanNotPresent) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";

    auto created = SSTableBuilder::create(path, 64);
    ASSERT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();
    EXPECT_OK(builder.add("alpha", "one", false));
    EXPECT_OK(builder.add("bravo", "", true));
    EXPECT_OK(builder.add("charlie", "three", false));
    EXPECT_OK(builder.finish());

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->entry_count(), 3u);

    std::string value;
    auto deleted = table->lookup("bravo", &value);
    ASSERT_TRUE(deleted.is_ok());
    // The distinction that stops an older table's value from surfacing.
    EXPECT_EQ(*deleted, Lookup::Deleted);

    auto absent = table->lookup("delta", &value);
    ASSERT_TRUE(absent.is_ok());
    EXPECT_EQ(*absent, Lookup::NotPresent);
}

TEST(SSTableRead, EmptyValuesAndBinaryKeysRoundTrip) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    const std::string binary_key("k\0ey", 4);
    const std::string blob("a\0b\0c", 5);

    auto created = SSTableBuilder::create(path, 4096);
    ASSERT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();
    EXPECT_OK(builder.add("empty", "", false));
    EXPECT_OK(builder.add(binary_key, blob, false));
    EXPECT_OK(builder.finish());

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);

    std::string value = "sentinel";
    auto empty = table->lookup("empty", &value);
    ASSERT_TRUE(empty.is_ok());
    EXPECT_EQ(*empty, Lookup::Found);
    EXPECT_EQ(value, "");

    auto binary = table->lookup(binary_key, &value);
    ASSERT_TRUE(binary.is_ok());
    EXPECT_EQ(*binary, Lookup::Found);
    EXPECT_EQ(value, blob);
    EXPECT_EQ(value.size(), 5u);
}

// --- Damage ----------------------------------------------------------------

TEST(SSTableCorruption, AFlippedByteInADataBlockFailsThatBlocksCrc) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 100, 64);

    // Byte 20 is inside the first data block, well past its first entry header.
    flip_byte_at(path, 20);

    auto table = SSTable::open(path, 1);
    // Opening reads block 0 for min_key, so the damage is caught right there --
    // which is the point of reading it at open rather than on the first get().
    EXPECT_STATUS(StatusCode::Corruption, table.status());
}

TEST(SSTableCorruption, AFlippedByteInALaterBlockIsCaughtOnTheReadThatTouchesIt) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 200, 64);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    ASSERT_GT(table->block_count(), 3u);

    const auto& target = table->index()[2];
    const std::uint64_t offset = target.block_offset + 12;
    table.reset();  // Windows will not let us write to a file we hold open.
    flip_byte_at(path, offset);

    auto reopened = open_table_or_fail(path);
    ASSERT_NE(reopened, nullptr) << "block 0 is undamaged, so the table still opens";

    std::string value;
    EXPECT_STATUS(StatusCode::Corruption, reopened->read_block(2).status());
}

TEST(SSTableCorruption, AFileTooSmallToHoldAFooterIsRejected) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 10);
    std::filesystem::resize_file(path, 8);

    EXPECT_STATUS(StatusCode::Corruption, SSTable::open(path, 1).status());
}

TEST(SSTableCorruption, ATruncatedTableIsRejectedRatherThanReadPartially) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 100, 64);
    truncate_by(path, 10);  // Cuts into the footer.

    EXPECT_STATUS(StatusCode::Corruption, SSTable::open(path, 1).status());
}

TEST(SSTableCorruption, AFlippedByteInTheIndexBlockFailsItsCrc) {
    TempDir dir;
    const auto path = dir.path() / "t.sst";
    build_table(path, 100, 64);

    auto table = open_table_or_fail(path);
    ASSERT_NE(table, nullptr);
    // Somewhere inside the index block: just before the footer.
    const std::uint64_t offset = std::filesystem::file_size(path) - sstable::kFooterSize - 6;
    table.reset();
    flip_byte_at(path, offset);

    EXPECT_STATUS(StatusCode::Corruption, SSTable::open(path, 1).status());
}

TEST(SSTableCorruption, AMissingFileIsAnIOErrorAndIsNotCreated) {
    TempDir dir;
    const auto path = dir.path() / "absent.sst";

    EXPECT_STATUS(StatusCode::IOError, SSTable::open(path, 1).status());
    EXPECT_FALSE(std::filesystem::exists(path))
        << "a failed open must not leave an empty file behind";
}

}  // namespace
}  // namespace kvstore
