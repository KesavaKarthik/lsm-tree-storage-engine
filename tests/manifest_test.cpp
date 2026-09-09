#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "kvstore/manifest.hpp"
#include "kvstore/record.hpp"
#include "kvstore_contract.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::TempDir;
using testing_support::temp_file_names;

manifest::TableMeta table(FileId id, std::string min_key, std::string max_key,
                          std::uint64_t size = 1000) {
    manifest::TableMeta meta;
    meta.id = id;
    meta.file_size = size;
    meta.entry_count = 42;
    meta.min_key = std::move(min_key);
    meta.max_key = std::move(max_key);
    return meta;
}

manifest::LevelSet sample() {
    manifest::LevelSet set;
    set.next_id = 9;
    set.levels.resize(3);
    set.levels[0] = {table(7, "a", "z"), table(5, "b", "y")};
    set.levels[1] = {table(3, "a", "m", 5000), table(4, "n", "z", 6000)};
    return set;
}

// --- Encoding --------------------------------------------------------------

TEST(ManifestFormat, RoundTripsALevelSet) {
    const manifest::LevelSet original = sample();
    auto decoded = manifest::decode(manifest::encode(original));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();

    EXPECT_EQ(decoded->next_id, 9u);
    ASSERT_EQ(decoded->levels.size(), 3u);
    ASSERT_EQ(decoded->levels[0].size(), 2u);
    ASSERT_EQ(decoded->levels[1].size(), 2u);
    EXPECT_TRUE(decoded->levels[2].empty());

    EXPECT_EQ(decoded->levels[0][0].id, 7u);
    EXPECT_EQ(decoded->levels[0][0].min_key, "a");
    EXPECT_EQ(decoded->levels[0][0].max_key, "z");
    EXPECT_EQ(decoded->levels[1][1].id, 4u);
    EXPECT_EQ(decoded->levels[1][1].file_size, 6000u);
    EXPECT_EQ(decoded->levels[1][1].entry_count, 42u);
}

TEST(ManifestFormat, RoundTripsAnEmptyLevelSet) {
    manifest::LevelSet empty;
    auto decoded = manifest::decode(manifest::encode(empty));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->next_id, kFirstFileId);
    EXPECT_EQ(decoded->table_count(), 0u);
}

TEST(ManifestFormat, KeysAreBinarySafe) {
    manifest::LevelSet set;
    set.levels.resize(1);
    set.levels[0] = {table(1, std::string("a\0b", 3), std::string("z\0\0", 3))};

    auto decoded = manifest::decode(manifest::encode(set));
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->levels[0][0].min_key, std::string("a\0b", 3));
    EXPECT_EQ(decoded->levels[0][0].max_key.size(), 3u);
}

TEST(ManifestFormat, ComputesLevelSizes) {
    const manifest::LevelSet set = sample();
    EXPECT_EQ(set.table_count(), 4u);
    EXPECT_EQ(set.level_bytes(0), 2000u);
    EXPECT_EQ(set.level_bytes(1), 11000u);
    EXPECT_EQ(set.level_bytes(99), 0u) << "a level that does not exist holds nothing";
    EXPECT_EQ(set.total_bytes(), 13000u);
}

TEST(ManifestFormat, OverlapIsInclusiveAtBothEnds) {
    const manifest::TableMeta meta = table(1, "d", "h");
    EXPECT_TRUE(meta.overlaps("a", "d")) << "min_key is a key that exists";
    EXPECT_TRUE(meta.overlaps("h", "z")) << "and so is max_key";
    EXPECT_TRUE(meta.overlaps("e", "f"));
    EXPECT_TRUE(meta.overlaps("a", "z"));
    EXPECT_FALSE(meta.overlaps("a", "c"));
    EXPECT_FALSE(meta.overlaps("i", "z"));
}

// --- Damage ----------------------------------------------------------------

TEST(ManifestFormat, AFlippedByteFailsTheCrc) {
    std::vector<std::uint8_t> bytes = manifest::encode(sample());
    bytes[20] ^= 0xFF;
    EXPECT_STATUS(StatusCode::Corruption, manifest::decode(bytes).status());
}

TEST(ManifestFormat, BadMagicIsRejectedBeforeAnythingElse) {
    std::vector<std::uint8_t> bytes = manifest::encode(sample());
    bytes[0] = 'X';
    EXPECT_STATUS(StatusCode::Corruption, manifest::decode(bytes).status());
}

TEST(ManifestFormat, AnUnknownFormatVersionIsRejected) {
    std::vector<std::uint8_t> bytes = manifest::encode(sample());
    bytes[4] = 99;  // format_version

    // Repair the crc, so it is the version check that fires rather than the sum.
    const std::size_t payload = bytes.size() - manifest::kTrailerSize;
    const std::uint32_t crc =
        record::crc32_of(std::span<const std::uint8_t>{bytes.data(), payload});
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[payload + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
    EXPECT_STATUS(StatusCode::Corruption, manifest::decode(bytes).status());
}

TEST(ManifestFormat, ATruncatedManifestIsRejected) {
    std::vector<std::uint8_t> bytes = manifest::encode(sample());
    bytes.resize(bytes.size() / 2);
    EXPECT_STATUS(StatusCode::Corruption, manifest::decode(bytes).status());
}

TEST(ManifestFormat, SomethingFarTooSmallIsRejected) {
    const std::vector<std::uint8_t> bytes(4, 0);
    EXPECT_STATUS(StatusCode::Corruption, manifest::decode(bytes).status());
}

// --- On disk ---------------------------------------------------------------

TEST(ManifestFile, ADirectoryWithoutOneReportsAbsenceRatherThanFailure) {
    TempDir dir;
    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok()) << set.status().to_string();
    EXPECT_FALSE(set->has_value()) << "a missing manifest is a fact, not an error";
}

TEST(ManifestFile, WritesAndReadsBackThroughTheFilesystem) {
    TempDir dir;
    EXPECT_OK(manifest::write_atomic(dir.path(), sample()));

    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok()) << set.status().to_string();
    ASSERT_TRUE(set->has_value());
    EXPECT_EQ((*set)->next_id, 9u);
    EXPECT_EQ((*set)->table_count(), 4u);
}

TEST(ManifestFile, LeavesNoTempBehind) {
    TempDir dir;
    EXPECT_OK(manifest::write_atomic(dir.path(), sample()));
    EXPECT_TRUE(temp_file_names(dir).empty())
        << "the temp must have been renamed, not abandoned";
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "MANIFEST"));
}

// The whole point of writing it whole and renaming: the second write replaces
// the first completely, with no moment at which a reader sees a blend.
TEST(ManifestFile, RewritingReplacesTheContentsEntirely) {
    TempDir dir;
    EXPECT_OK(manifest::write_atomic(dir.path(), sample()));

    manifest::LevelSet smaller;
    smaller.next_id = 100;
    smaller.levels.resize(1);
    smaller.levels[0] = {table(1, "a", "b")};
    EXPECT_OK(manifest::write_atomic(dir.path(), smaller));

    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok());
    ASSERT_TRUE(set->has_value());
    EXPECT_EQ((*set)->next_id, 100u);
    EXPECT_EQ((*set)->table_count(), 1u) << "no trace of the four-table version";
}

TEST(ManifestFile, ADamagedManifestOnDiskIsCorruptionNotAbsence) {
    TempDir dir;
    EXPECT_OK(manifest::write_atomic(dir.path(), sample()));
    testing_support::flip_byte_at(dir.path() / "MANIFEST", 20);

    auto set = manifest::read(dir.path());
    EXPECT_STATUS(StatusCode::Corruption, set.status());
}

TEST(ManifestFile, AStaleTempIsReplacedRatherThanAppendedTo) {
    TempDir dir;
    testing_support::append_raw(dir.path() / "MANIFEST.tmp",
                                std::vector<std::uint8_t>(500, 0xAB));

    EXPECT_OK(manifest::write_atomic(dir.path(), sample()));
    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok()) << set.status().to_string();
    ASSERT_TRUE(set->has_value());
    EXPECT_EQ((*set)->table_count(), 4u);
}

}  // namespace
}  // namespace kvstore
