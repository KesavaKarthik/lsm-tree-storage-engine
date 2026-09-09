#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "kvstore/lsm.hpp"
#include "kvstore/sstable.hpp"
#include "kvstore_contract.hpp"
#include "lsm_helpers.hpp"
#include "mapped_file.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::append_raw;
using testing_support::TempDir;

std::vector<std::uint8_t> pattern(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    }
    return bytes;
}

std::filesystem::path write_file(const TempDir& dir, const std::string& name,
                                 const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path path = dir.path() / name;
    append_raw(path, bytes);
    return path;
}

// --- The mapping itself ----------------------------------------------------

TEST(MappedFileTest, MapsAWholeFileAndReadsItBack) {
    TempDir dir;
    const std::vector<std::uint8_t> bytes = pattern(10000);
    const auto path = write_file(dir, "data.bin", bytes);

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok()) << mapped.status().to_string();

    EXPECT_EQ(mapped->size(), bytes.size());
    ASSERT_EQ(mapped->bytes().size(), bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), mapped->bytes().begin()));
}

TEST(MappedFileTest, ReadsAtEveryOffsetIncludingTheLastByte) {
    TempDir dir;
    const std::vector<std::uint8_t> bytes = pattern(4097);  // Deliberately not page-aligned.
    const auto path = write_file(dir, "odd.bin", bytes);

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok()) << mapped.status().to_string();

    const std::span<const std::uint8_t> view = mapped->bytes();
    ASSERT_EQ(view.size(), 4097u);
    EXPECT_EQ(view[0], bytes[0]);
    EXPECT_EQ(view[4096], bytes[4096]) << "the last byte of a file that ends mid-page";
}

// The property that lets a mapped table cost no file descriptor: the mapping
// holds its own reference, so open() closes the fd immediately and the bytes
// stay readable regardless.
TEST(MappedFileTest, TheMappingOutlivesTheDescriptorItWasMadeFrom) {
    TempDir dir;
    const std::vector<std::uint8_t> bytes = pattern(1000);
    const auto path = write_file(dir, "data.bin", bytes);

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok());
    // open() already closed the descriptor. If that had invalidated the mapping,
    // this read would fault rather than return.
    EXPECT_EQ(mapped->bytes()[999], bytes[999]);
}

TEST(MappedFileTest, AdviseRandomSucceedsOnAMappedFile) {
    TempDir dir;
    const auto path = write_file(dir, "data.bin", pattern(8192));

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok());
    EXPECT_OK(mapped->advise_random());
}

// --- Lifetime --------------------------------------------------------------

TEST(MappedFileTest, CloseIsIdempotentAndLeavesTheObjectUnmapped) {
    TempDir dir;
    const auto path = write_file(dir, "data.bin", pattern(1000));

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok());
    ASSERT_TRUE(mapped->is_mapped());

    EXPECT_OK(mapped->close());
    EXPECT_FALSE(mapped->is_mapped());
    // Closing twice must be harmless -- the second call has nothing to release.
    EXPECT_OK(mapped->close());
}

TEST(MappedFileTest, MovingTransfersTheMappingAndLeavesTheSourceEmpty) {
    TempDir dir;
    const auto path = write_file(dir, "data.bin", pattern(1000));

    auto opened = MappedFile::open(path);
    ASSERT_TRUE(opened.is_ok());
    MappedFile source = opened.take();
    ASSERT_TRUE(source.is_mapped());

    MappedFile moved = std::move(source);
    EXPECT_TRUE(moved.is_mapped());
    EXPECT_FALSE(source.is_mapped()) << "a moved-from mapping must not unmap twice";
    EXPECT_EQ(moved.bytes().size(), 1000u);
}

// The Windows rule that shapes SSTable's destructor: a file with a live mapping
// cannot be removed, exactly as one with an open handle cannot.
TEST(MappedFileTest, TheFileCanBeDeletedOnceTheMappingIsReleased) {
    TempDir dir;
    const auto path = write_file(dir, "data.bin", pattern(1000));

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok());
    EXPECT_OK(mapped->close());

    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_FALSE(ec) << "unmapping first is what makes the unlink legal";
    EXPECT_FALSE(std::filesystem::exists(path));
}

// --- Refusals --------------------------------------------------------------

TEST(MappedFileTest, AMissingFileIsAnIOErrorAndIsNotCreated) {
    TempDir dir;
    const auto path = dir.path() / "absent.bin";

    EXPECT_STATUS(StatusCode::IOError, MappedFile::open(path).status());
    EXPECT_FALSE(std::filesystem::exists(path))
        << "a failed map must not leave an empty file behind";
}

// Neither platform can map zero bytes, and saying so is better than returning a
// mapping of nothing that faults on first touch.
TEST(MappedFileTest, AnEmptyFileIsRejectedRatherThanMappedToNothing) {
    TempDir dir;
    const auto path = write_file(dir, "empty.bin", {});
    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_EQ(std::filesystem::file_size(path), 0u);

    EXPECT_STATUS(StatusCode::InvalidArgument, MappedFile::open(path).status());
}

TEST(MappedFileTest, AdviseOnAnUnmappedFileIsAnError) {
    TempDir dir;
    const auto path = write_file(dir, "data.bin", pattern(1000));

    auto mapped = MappedFile::open(path);
    ASSERT_TRUE(mapped.is_ok());
    EXPECT_OK(mapped->close());
    EXPECT_STATUS(StatusCode::IOError, mapped->advise_random());
}


// --- mmap vs pread: the two paths must be indistinguishable ----------------
//
// The whole justification for keeping both is that they differ only in cost. If
// they ever differ in *answers*, the faster one is not an optimisation, it is a
// second implementation of the read path with its own bugs.

std::string ab_key(int i) {
    std::string digits = std::to_string(i);
    return "key" + std::string(4 - digits.size(), '0') + digits;
}

std::filesystem::path build_ab_table(const TempDir& dir, int count) {
    const std::filesystem::path path = dir.path() / "ab.sst";
    auto created = SSTableBuilder::create(path, /*block_size=*/64);
    EXPECT_TRUE(created.is_ok());
    SSTableBuilder builder = created.take();
    for (int i = 0; i < count; ++i) {
        // Every fifth key is a tombstone, so the comparison covers all three
        // lookup outcomes rather than only the happy one.
        const bool tombstone = (i % 5 == 0);
        EXPECT_OK(builder.add(ab_key(i), tombstone ? "" : "value" + std::to_string(i),
                              tombstone));
    }
    EXPECT_OK(builder.finish());
    return path;
}

TEST(MmapVsPread, TableMetadataIsIdentical) {
    TempDir dir;
    const auto path = build_ab_table(dir, 300);

    auto by_pread = SSTable::open(path, 1, nullptr, ReadMode::Pread);
    auto by_mmap = SSTable::open(path, 1, nullptr, ReadMode::Mmap);
    ASSERT_TRUE(by_pread.is_ok()) << by_pread.status().to_string();
    ASSERT_TRUE(by_mmap.is_ok()) << by_mmap.status().to_string();

    EXPECT_EQ(by_pread->get()->entry_count(), by_mmap->get()->entry_count());
    EXPECT_EQ(by_pread->get()->block_count(), by_mmap->get()->block_count());
    EXPECT_EQ(by_pread->get()->file_size(), by_mmap->get()->file_size());
    EXPECT_EQ(by_pread->get()->min_key(), by_mmap->get()->min_key());
    EXPECT_EQ(by_pread->get()->max_key(), by_mmap->get()->max_key());
    ASSERT_GT(by_mmap->get()->block_count(), 10u) << "the test needs many blocks";
}

TEST(MmapVsPread, EveryBlockIsByteIdentical) {
    TempDir dir;
    const auto path = build_ab_table(dir, 300);

    auto by_pread = SSTable::open(path, 1, nullptr, ReadMode::Pread);
    auto by_mmap = SSTable::open(path, 1, nullptr, ReadMode::Mmap);
    ASSERT_TRUE(by_pread.is_ok());
    ASSERT_TRUE(by_mmap.is_ok());

    const std::unique_ptr<SSTable>& pread_table = *by_pread;
    const std::unique_ptr<SSTable>& mmap_table = *by_mmap;
    ASSERT_EQ(pread_table->block_count(), mmap_table->block_count());

    for (std::size_t i = 0; i < pread_table->block_count(); ++i) {
        SCOPED_TRACE("block " + std::to_string(i));
        auto a = pread_table->read_block(i);
        auto b = mmap_table->read_block(i);
        ASSERT_TRUE(a.is_ok()) << a.status().to_string();
        ASSERT_TRUE(b.is_ok()) << b.status().to_string();

        EXPECT_TRUE(a->owns()) << "the pread path must own its buffer";
        EXPECT_FALSE(b->owns()) << "the mapped path must borrow, or it copied for nothing";

        ASSERT_EQ(a->size(), b->size());
        EXPECT_TRUE(std::equal(a->bytes().begin(), a->bytes().end(), b->bytes().begin()));
    }
}

TEST(MmapVsPread, EveryLookupAgreesIncludingTombstonesAndMisses) {
    TempDir dir;
    const auto path = build_ab_table(dir, 300);

    auto by_pread = SSTable::open(path, 1, nullptr, ReadMode::Pread);
    auto by_mmap = SSTable::open(path, 1, nullptr, ReadMode::Mmap);
    ASSERT_TRUE(by_pread.is_ok());
    ASSERT_TRUE(by_mmap.is_ok());

    for (int i = -5; i < 320; ++i) {
        const std::string key = ab_key(i < 0 ? 9000 + i : i);
        SCOPED_TRACE(key);

        std::string pread_value = "sentinel-a";
        std::string mmap_value = "sentinel-b";
        auto a = (*by_pread)->lookup(key, &pread_value);
        auto b = (*by_mmap)->lookup(key, &mmap_value);
        ASSERT_TRUE(a.is_ok());
        ASSERT_TRUE(b.is_ok());

        EXPECT_EQ(static_cast<int>(*a), static_cast<int>(*b));
        if (*a == Lookup::Found) {
            EXPECT_EQ(pread_value, mmap_value);
        }
    }
}

// The same question one level up: an entire database, opened each way, must
// answer identically -- including through compaction, which rewrites the tables
// the second engine will then map.
TEST(MmapVsPread, AWholeDatabaseAnswersIdenticallyThroughEitherPath) {
    TempDir dir;
    constexpr int kCount = 600;

    {
        LsmOptions options = testing_support::leveled();
        options.read_mode = ReadMode::Pread;
        auto store = testing_support::open_lsm_or_fail(dir, options);
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < kCount; ++i) {
            EXPECT_OK(store->put(ab_key(i), "value" + std::to_string(i)));
        }
        for (int i = 0; i < kCount; i += 7) {
            EXPECT_OK(store->remove(ab_key(i)));
        }
        EXPECT_OK(store->flush());
        EXPECT_OK(store->compact());
    }

    const auto collect_answers = [&](ReadMode mode) {
        LsmOptions options = testing_support::leveled();
        options.read_mode = mode;
        auto store = testing_support::open_lsm_or_fail(dir, options);
        EXPECT_NE(store, nullptr);

        std::vector<std::string> answers;
        for (int i = 0; i < kCount + 50; ++i) {
            std::string value;
            const Status status = store->get(ab_key(i), &value);
            answers.push_back(status.is_ok() ? "ok:" + value
                                             : Status::code_to_string(status.code()));
        }

        auto opened = store->scan("", "");
        EXPECT_TRUE(opened.is_ok());
        Iterator cursor = opened.take();
        for (const auto& [key, value] : testing_support::collect(cursor)) {
            answers.push_back("scan:" + key + "=" + value);
        }
        return answers;
    };

    const std::vector<std::string> by_pread = collect_answers(ReadMode::Pread);
    const std::vector<std::string> by_mmap = collect_answers(ReadMode::Mmap);

    ASSERT_FALSE(by_pread.empty());
    EXPECT_EQ(by_pread, by_mmap)
        << "the two read paths are supposed to differ in cost, not in answers";
}

// A mapped table is subject to the same crc discipline. Nothing about handing
// back the kernel's page makes the bytes on it trustworthy.
TEST(MmapVsPread, ADamagedBlockIsCaughtOnBothPaths) {
    TempDir dir;
    const auto path = build_ab_table(dir, 300);

    // Somewhere inside block 2, found through the index rather than guessed.
    std::uint64_t offset = 0;
    {
        auto table = SSTable::open(path, 1, nullptr, ReadMode::Pread);
        ASSERT_TRUE(table.is_ok());
        ASSERT_GT((*table)->block_count(), 3u);
        offset = (*table)->index()[2].block_offset + 8;
    }
    testing_support::flip_byte_at(path, offset);

    auto by_pread = SSTable::open(path, 1, nullptr, ReadMode::Pread);
    auto by_mmap = SSTable::open(path, 1, nullptr, ReadMode::Mmap);
    ASSERT_TRUE(by_pread.is_ok()) << "block 0 is undamaged, so the table still opens";
    ASSERT_TRUE(by_mmap.is_ok());

    EXPECT_STATUS(StatusCode::Corruption, (*by_pread)->read_block(2).status());
    EXPECT_STATUS(StatusCode::Corruption, (*by_mmap)->read_block(2).status());
}

}  // namespace
}  // namespace kvstore
