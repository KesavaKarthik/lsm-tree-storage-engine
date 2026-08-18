#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/file_names.hpp"
#include "kvstore/hint.hpp"
#include "kvstore_contract.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::data_file_ids;
using testing_support::flip_byte_at;
using testing_support::hint_file_ids;
using testing_support::hint_path;
using testing_support::open_or_fail;
using testing_support::TempDir;
using testing_support::tiny_files;
using testing_support::truncate_by;

constexpr std::uint64_t kSmall = 80;

// --- The format on its own -------------------------------------------------

TEST(HintFormat, EntryRoundTrips) {
    const auto bytes = hint::encode_entry("alpha", 12345, 4096, 99);
    EXPECT_EQ(bytes.size(), hint::kEntryHeaderSize + 5);

    auto entry = hint::decode_entry(bytes);
    ASSERT_TRUE(entry.is_ok()) << entry.status().to_string();
    EXPECT_EQ(entry->key, "alpha");
    EXPECT_EQ(entry->timestamp, 12345u);
    EXPECT_EQ(entry->record_offset, 4096u);
    EXPECT_EQ(entry->record_size, 99u);
    EXPECT_EQ(entry->encoded_size(), bytes.size());
}

TEST(HintFormat, EntryKeysAreBinarySafe) {
    const std::string key("a\0b\xFF", 4);
    const auto bytes = hint::encode_entry(key, 1, 2, 64);
    auto entry = hint::decode_entry(bytes);
    ASSERT_TRUE(entry.is_ok());
    EXPECT_EQ(entry->key, key);
}

TEST(HintFormat, EntryCrcCatchesAFlippedByte) {
    auto bytes = hint::encode_entry("alpha", 1, 2, 64);
    bytes.back() ^= 0xFF;  // Last byte of the key.
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_entry(bytes).status());
}

TEST(HintFormat, EntryCrcCoversTheOffsetToo) {
    // The offset is the whole point of the entry: a corrupt one sends a reader
    // to the wrong place in the data file, where it would find a record for a
    // different key. Nothing but the crc can catch that here.
    auto bytes = hint::encode_entry("alpha", 1, 2, 64);
    bytes[12] ^= 0xFF;  // First byte of record_offset.
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_entry(bytes).status());
}

TEST(HintFormat, TruncatedEntryIsRejected) {
    const auto bytes = hint::encode_entry("alpha", 1, 2, 64);
    for (std::size_t cut = 1; cut <= bytes.size(); ++cut) {
        SCOPED_TRACE("truncated to " + std::to_string(bytes.size() - cut));
        const std::vector<std::uint8_t> partial(bytes.begin(),
                                                bytes.end() - static_cast<long>(cut));
        EXPECT_STATUS(StatusCode::Corruption, hint::decode_entry(partial).status());
    }
}

TEST(HintFormat, ZeroLengthKeyIsRejected) {
    // Same rule as the record format: an empty key is the signature of a
    // corrupt header, not something any writer produces.
    std::vector<std::uint8_t> bytes(hint::kEntryHeaderSize, 0);
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_entry(bytes).status());
}

TEST(HintFormat, ImpossiblySmallRecordSizeIsRejected) {
    // A record cannot be smaller than a record header, so an entry claiming so
    // is damaged -- and its size would go straight into a read.
    const auto bytes = hint::encode_entry("alpha", 1, 2, /*record_size=*/3);
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_entry(bytes).status());
}

TEST(HintFormat, HostileKeySizeDoesNotReadPastTheBuffer) {
    // Memory-safety case, meaningful under ASan: an exactly-sized heap buffer
    // whose header claims a far larger key. The bounds check has to fire before
    // anything reads the key bytes.
    auto bytes = hint::encode_entry("alpha", 1, 2, 64);
    bytes[24] = 0xFF;
    bytes[25] = 0xFF;
    bytes[26] = 0xFF;
    bytes[27] = 0x7F;
    const std::vector<std::uint8_t> exact(bytes.begin(), bytes.end());
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_entry(exact).status());
}

TEST(HintFormat, FooterRoundTrips) {
    const auto bytes = hint::encode_footer(4096, 17);
    EXPECT_EQ(bytes.size(), hint::kFooterSize);

    auto footer = hint::decode_footer(bytes);
    ASSERT_TRUE(footer.is_ok()) << footer.status().to_string();
    EXPECT_EQ(footer->data_file_size, 4096u);
    EXPECT_EQ(footer->entry_count, 17u);
}

TEST(HintFormat, FooterMagicIsChecked) {
    auto bytes = hint::encode_footer(4096, 17);
    bytes[0] = 'X';
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_footer(bytes).status());
}

TEST(HintFormat, FooterCrcIsChecked) {
    auto bytes = hint::encode_footer(4096, 17);
    bytes[4] ^= 0xFF;  // First byte of data_file_size.
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_footer(bytes).status());
}

TEST(HintFormat, FooterOfTheWrongSizeIsRejected) {
    auto bytes = hint::encode_footer(4096, 17);
    bytes.pop_back();
    EXPECT_STATUS(StatusCode::Corruption, hint::decode_footer(bytes).status());
}

// --- Recovery through a hint -----------------------------------------------

// Sorted index snapshot, so two recoveries can be compared exactly.
std::vector<std::pair<std::string, ValuePointer>> sorted_index(const Bitcask& store) {
    auto entries = store.index_snapshot();
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return entries;
}

void expect_same_index(const std::vector<std::pair<std::string, ValuePointer>>& a,
                       const std::vector<std::pair<std::string, ValuePointer>>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        SCOPED_TRACE("entry " + std::to_string(i) + " (" + a[i].first + ")");
        EXPECT_EQ(a[i].first, b[i].first);
        EXPECT_EQ(a[i].second.file_id, b[i].second.file_id);
        EXPECT_EQ(a[i].second.offset, b[i].second.offset);
        EXPECT_EQ(a[i].second.size, b[i].second.size);
        EXPECT_EQ(a[i].second.timestamp, b[i].second.timestamp);
    }
}

// Builds a compacted database and returns the id of the merged file.
FileId build_compacted(const TempDir& dir) {
    auto store = open_or_fail(dir, tiny_files(kSmall));
    EXPECT_NE(store, nullptr);
    for (int i = 0; i < 30; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    for (int i = 0; i < 30; i += 3) {
        EXPECT_OK(store->put("key" + std::to_string(i), "rewritten"));
    }
    for (int i = 1; i < 30; i += 7) {
        EXPECT_OK(store->remove("key" + std::to_string(i)));
    }
    EXPECT_OK(store->compact());
    return data_file_ids(dir).front();
}

TEST(HintRecovery, CompactionWritesAHintBesideTheMergedFile) {
    TempDir dir;
    const FileId merged = build_compacted(dir);

    // Exactly one hint, for the merged file. The active file never gets one --
    // it is still being appended to, so nothing about it would stay true.
    EXPECT_EQ(hint_file_ids(dir), (std::vector<FileId>{merged}));
    EXPECT_TRUE(std::filesystem::exists(hint_path(dir, merged)));
}

TEST(HintRecovery, AHintIsSizedByItsKeysNotItsValues) {
    // Where the speedup actually comes from, and its limit.
    //
    // A hint entry is 28 bytes plus the key, whatever the value is; a record is
    // 21 bytes plus the key *plus the value*. So the saving is the value bytes,
    // and it is worth having exactly in proportion to how large values are.
    // With short values a hint is no smaller than the data -- the 28-byte entry
    // header is bigger than the 21-byte record header -- which is worth knowing
    // before concluding hint files are a free win.
    TempDir dir;
    const std::string big_value(400, 'v');
    {
        auto store = open_or_fail(dir, tiny_files(4096));
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < 30; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), big_value));
        }
        EXPECT_OK(store->compact());
    }

    const FileId merged = data_file_ids(dir).front();
    const auto hint_size = std::filesystem::file_size(hint_path(dir, merged));
    const auto data_size = std::filesystem::file_size(dir.path() / log_file_name(merged));

    // Recovery reads the small one instead of the large one.
    EXPECT_LT(hint_size * 10, data_size);
}

TEST(HintRecovery, RebuildsExactlyTheIndexAFullScanWould) {
    // The property that matters: a hint is a faster route to the *same* answer,
    // not an approximation of it. Compared entry by entry on file id, offset,
    // size, and timestamp -- not merely on the values that come back.
    TempDir dir;
    const FileId merged = build_compacted(dir);

    std::vector<std::pair<std::string, ValuePointer>> from_hint;
    {
        auto store = open_or_fail(dir, tiny_files(kSmall));
        ASSERT_NE(store, nullptr);
        from_hint = sorted_index(*store);
    }
    ASSERT_FALSE(from_hint.empty());

    // Take the hint away and make recovery do it the slow way.
    std::filesystem::remove(hint_path(dir, merged));

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);
    expect_same_index(from_hint, sorted_index(*store));
}

// Damages the hint in some way, then checks recovery still produces the index a
// full scan would -- silently, because a hint is a cache and losing one is not
// an error.
//
// The baseline is taken from *this same database* before the damage, not from a
// second one built the same way: record timestamps come from the clock, so two
// separately-built databases never have identical index entries.
void expect_falls_back_to_full_scan(void (*damage)(const std::filesystem::path&)) {
    TempDir dir;
    const FileId merged = build_compacted(dir);

    std::vector<std::pair<std::string, ValuePointer>> expected;
    {
        auto store = open_or_fail(dir, tiny_files(kSmall));
        ASSERT_NE(store, nullptr);
        expected = sorted_index(*store);
    }
    ASSERT_FALSE(expected.empty());

    damage(hint_path(dir, merged));

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);
    expect_same_index(expected, sorted_index(*store));

    // The database is fully usable afterwards, not merely openable.
    std::string value;
    EXPECT_OK(store->get("key2", &value));
    EXPECT_EQ(value, "value2");
}

TEST(HintRecovery, AHintWithNoFooterIsIgnored) {
    // The footer is the hint's commit record. Without it the file is one an
    // interrupted write could have produced, and nothing in it can be trusted.
    expect_falls_back_to_full_scan(
        [](const std::filesystem::path& path) { truncate_by(path, hint::kFooterSize); });
}

TEST(HintRecovery, AHintTruncatedMidEntryIsIgnored) {
    expect_falls_back_to_full_scan(
        [](const std::filesystem::path& path) { truncate_by(path, 5); });
}

TEST(HintRecovery, AHintWhoseFooterDisagreesAboutTheDataFileSizeIsIgnored) {
    // The check that stops a hint from ever outliving the file it describes.
    //
    // A *well-formed* footer with the wrong length in it, not a corrupt one --
    // otherwise the crc would fire first and this would be testing that check
    // over again. This is what a stale hint left beside a rewritten data file
    // would look like, and its offsets would point into unrelated bytes.
    expect_falls_back_to_full_scan([](const std::filesystem::path& path) {
        const auto size = std::filesystem::file_size(path);
        std::vector<std::uint8_t> footer(hint::kFooterSize);
        {
            std::ifstream in(path, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(size - hint::kFooterSize));
            in.read(reinterpret_cast<char*>(footer.data()),
                    static_cast<std::streamsize>(footer.size()));
        }
        auto decoded = hint::decode_footer(footer);
        ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();

        testing_support::overwrite_at(
            path, size - hint::kFooterSize,
            hint::encode_footer(decoded->data_file_size + 1, decoded->entry_count));
    });
}

TEST(HintRecovery, AFlippedByteInAnEntryDiscardsTheWholeHint) {
    // Not just the damaged entry: a hint is applied all or nothing, because a
    // half-applied one would leave entries from a file we just decided not to
    // trust, for keys that file may not even contain.
    expect_falls_back_to_full_scan(
        [](const std::filesystem::path& path) { flip_byte_at(path, 20); });
}

TEST(HintRecovery, AnEmptyHintFileIsIgnored) {
    expect_falls_back_to_full_scan([](const std::filesystem::path& path) {
        std::filesystem::resize_file(path, 0);
    });
}

TEST(HintRecovery, AHintLeftBehindWithoutItsDataFileIsRemoved) {
    TempDir dir;
    {
        auto store = open_or_fail(dir, tiny_files(kSmall));
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
    }

    // What a crash between deleting a data file and deleting its hint leaves.
    const auto orphan = hint_path(dir, 9);
    testing_support::append_raw(orphan, hint::encode_footer(0, 0));
    ASSERT_TRUE(std::filesystem::exists(orphan));

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);
    EXPECT_FALSE(std::filesystem::exists(orphan));
    EXPECT_EQ(store->key_count(), 1u);
}

TEST(HintRecovery, AHintMeansTheDataFileIsNotReadAtStartup) {
    // Proof that the hint path is really being taken -- every other test here
    // would still pass if replay_hint always gave up and fell back.
    //
    // It also pins the trade honestly. Damage a value byte in the merged file:
    //
    //   with the hint    -- open() succeeds, because nothing read those bytes.
    //   without the hint -- open() returns Corruption, because the full scan
    //                       checks every record's crc on the way past.
    //
    // Startup crc verification is what a hint file buys its speed with. The
    // damage is still caught, just at the first get() of that key instead of at
    // open, so no wrong answer ever escapes -- the detection simply moves.
    TempDir dir;
    const FileId merged = build_compacted(dir);

    const auto data_file = dir.path() / log_file_name(merged);
    const auto data_size = std::filesystem::file_size(data_file);
    ASSERT_GT(data_size, 100u);
    flip_byte_at(data_file, data_size / 2);

    {
        auto store = open_or_fail(dir, tiny_files(kSmall));
        ASSERT_NE(store, nullptr);  // Opened: the data file was never scanned.

        // And the damage is still caught, on the read of whichever key it hit.
        int corrupt = 0;
        std::string value;
        for (int i = 0; i < 30; ++i) {
            if (store->get("key" + std::to_string(i), &value).is_corruption()) {
                ++corrupt;
            }
        }
        EXPECT_GT(corrupt, 0) << "a damaged record was not caught on read";
    }

    // Take the hint away and the same database refuses to open at all.
    std::filesystem::remove(hint_path(dir, merged));
    auto rescanned = Bitcask::open(dir.path(), tiny_files(kSmall));
    EXPECT_STATUS(StatusCode::Corruption, rescanned.status());
}

TEST(HintRecovery, WritesAfterAHintBasedRecoveryAreCorrect) {
    // Recovering through a hint has to leave the engine in exactly the state a
    // full scan would -- including whatever it needs to keep writing.
    TempDir dir;
    build_compacted(dir);

    {
        auto store = open_or_fail(dir, tiny_files(kSmall));
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("key0", "after-hint-recovery"));
        EXPECT_OK(store->put("brand-new", "value"));
        EXPECT_OK(store->remove("key2"));
    }

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    std::string value;
    EXPECT_OK(store->get("key0", &value));
    EXPECT_EQ(value, "after-hint-recovery");
    EXPECT_OK(store->get("brand-new", &value));
    EXPECT_EQ(value, "value");
    EXPECT_STATUS(StatusCode::NotFound, store->get("key2", &value));
}

TEST(HintRecovery, CompactingTwiceReplacesTheHintRatherThanStrandingIt) {
    TempDir dir;
    const FileId merged = build_compacted(dir);

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);
    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "again"));
    }
    EXPECT_OK(store->compact());

    // Still exactly one hint, still beside the merged file, and still describing
    // it -- a stale hint here would point at offsets in a file that no longer
    // has that shape.
    EXPECT_EQ(hint_file_ids(dir), (std::vector<FileId>{merged}));
    store.reset();

    auto reopened = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(reopened, nullptr);
    std::string value;
    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(reopened->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "again");
    }
}

}  // namespace
}  // namespace kvstore
