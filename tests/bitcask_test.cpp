#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/file_names.hpp"
#include "kvstore/record.hpp"
#include "kvstore_contract.hpp"
#include "raw_record.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::append_raw;
using testing_support::data_file_ids;
using testing_support::data_path;
using testing_support::flip_byte_at;
using testing_support::make_raw_header;
using testing_support::open_or_fail;
using testing_support::TempDir;
using testing_support::tiny_files;

// --- The shared contract, now against real files ---------------------------

struct BitcaskFactory {
    std::unique_ptr<KVStore> create() {
        // Never: these cases test semantics, not durability, and an fsync per
        // put makes the suite needlessly slow. The durability tests below opt
        // into Always.
        auto store = Bitcask::open(dir.path(), BitcaskOptions{SyncMode::Never});
        EXPECT_TRUE(store.is_ok()) << store.status().to_string();
        return store.is_ok() ? std::unique_ptr<KVStore>{store.take()} : nullptr;
    }

    TempDir dir;
};

INSTANTIATE_TYPED_TEST_SUITE_P(Bitcask, KVStoreContract, BitcaskFactory);

// --- Helpers ---------------------------------------------------------------

// The first data file. Most cases here predate rotation and stay in it: the
// default threshold is 64MiB and these tests write bytes.
std::filesystem::path log_path(const TempDir& dir) { return data_path(dir, kFirstFileId); }

// --- Persistence -----------------------------------------------------------

TEST(BitcaskPersistence, ValuesSurviveReopen) {
    TempDir dir;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        EXPECT_OK(store->put("gamma", "three"));
    }  // Destructor closes the fd. Nothing else is done to "save".

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 3u);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
    EXPECT_OK(store->get("gamma", &value));
    EXPECT_EQ(value, "three");
}

TEST(BitcaskPersistence, DeletesSurviveReopen) {
    TempDir dir;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        EXPECT_OK(store->remove("alpha"));
    }

    // The tombstone has to be replayed, not just the puts -- otherwise the
    // delete is undone by every restart.
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 1u);

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("alpha", &value));
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
}

TEST(BitcaskPersistence, NewestValueWinsAfterReopen) {
    TempDir dir;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "first"));
        EXPECT_OK(store->put("alpha", "second"));
        EXPECT_OK(store->put("alpha", "third"));
    }

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    // All three records are still on disk; only the last one is live.
    EXPECT_EQ(store->key_count(), 1u);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "third");
}

TEST(BitcaskPersistence, DeletedThenRewrittenKeyComesBack) {
    // put -> remove -> put must end live. Gets the tombstone ordering wrong if
    // replay applies deletes after puts instead of in file order.
    TempDir dir;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->remove("alpha"));
        EXPECT_OK(store->put("alpha", "two"));
    }

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "two");
}

TEST(BitcaskPersistence, EmptyDirectoryOpensClean) {
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 0u);
    EXPECT_EQ(store->active_file_size(), 0u);

    // A fresh database is one empty file, not zero: there has to be somewhere
    // for the first append to go.
    EXPECT_EQ(store->file_count(), 1u);
    EXPECT_EQ(store->active_file_id(), kFirstFileId);
    EXPECT_TRUE(std::filesystem::exists(log_path(dir)));
}

// --- Torn tail (a crash partway through a write) ---------------------------

TEST(BitcaskRecovery, DropsGarbageAppendedAfterGoodRecords) {
    TempDir dir;
    std::uint64_t good_size = 0;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        good_size = store->active_file_size();
    }

    append_raw(log_path(dir), {0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01});
    ASSERT_GT(std::filesystem::file_size(log_path(dir)), good_size);

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->key_count(), 2u);
    // The garbage is not merely ignored -- it is cut off, so the next append
    // doesn't write a good record on top of it.
    EXPECT_EQ(store->active_file_size(), good_size);
    EXPECT_EQ(std::filesystem::file_size(log_path(dir)), good_size);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
}

TEST(BitcaskRecovery, DropsAHalfWrittenRecord) {
    // The realistic crash: a complete header claiming a size the body never
    // reached. More dangerous than random garbage, because the header parses.
    TempDir dir;
    std::uint64_t good_size = 0;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        good_size = store->active_file_size();
    }

    auto partial = record::encode("beta", "two-that-never-landed", 999, false);
    partial.resize(partial.size() - 5);  // Power cut, five bytes short.
    append_raw(log_path(dir), partial);

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->key_count(), 1u);
    EXPECT_EQ(store->active_file_size(), good_size);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_STATUS(StatusCode::NotFound, store->get("beta", &value));
}

TEST(BitcaskRecovery, DropsARecordWhoseCrcFails) {
    // Full-length record, complete header, right size -- but the bytes are
    // wrong. Only the crc can tell.
    TempDir dir;
    std::uint64_t good_size = 0;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        good_size = store->active_file_size();
    }

    auto damaged = record::encode("beta", "two", 999, false);
    damaged.back() ^= 0xFF;
    append_raw(log_path(dir), damaged);

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->key_count(), 1u);
    EXPECT_EQ(store->active_file_size(), good_size);
}

TEST(BitcaskRecovery, WritesAfterRecoveryLandOnSolidGround) {
    // The property truncation exists for: once the tail is cut, new writes must
    // survive the *next* restart. If we had merely stopped reading instead of
    // truncating, this record would sit after garbage and vanish on reopen.
    TempDir dir;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
    }

    append_raw(log_path(dir), {0x01, 0x02, 0x03, 0x04, 0x05});

    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("beta", "two"));
    }

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 2u);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
}

TEST(BitcaskRecovery, SurvivesATornTombstone) {
    TempDir dir;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
    }

    auto partial_tombstone = record::encode("alpha", {}, 999, true);
    partial_tombstone.resize(partial_tombstone.size() - 2);
    append_raw(log_path(dir), partial_tombstone);

    // The delete never completed, so the key is still there. Applying a
    // half-written tombstone would lose data that was never asked to be lost.
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
}

TEST(BitcaskRecovery, RejectsAHeaderClaimingAnAbsurdLength) {
    // A corrupt header claiming a ~4GB key. The `offset + total > file_size`
    // bound must reject it against the real file length *before* anything is
    // allocated. Fuzzing measured peak RSS at 49MB across 958 such files, which
    // is what this test locks in: the check prevents the allocation, it doesn't
    // merely report afterwards.
    TempDir dir;
    std::uint64_t good_size = 0;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        good_size = store->active_file_size();
    }

    append_raw(log_path(dir), make_raw_header(0xFFFFFF00u, 0));

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 1u);
    EXPECT_EQ(store->active_file_size(), good_size);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
}

TEST(BitcaskRecovery, RejectsAHeaderLargerThanARecordMayBe) {
    // ~8.6GB, which overflows the uint32_t recover() narrows `total` into.
    //
    // Honest scope: this passes with or without the kMaxRecordSize check,
    // because on a small file the `offset + total > file_size` bound rejects the
    // header first. The narrowing only bites on a >4GB log, which a test cannot
    // practically create. RecordTest.ImpossiblyLargeTotalIsRejected is the case
    // that actually guards the fix -- verified by disabling the check and
    // confirming it was the only failure. This one covers the layer above.
    TempDir dir;
    std::uint64_t good_size = 0;
    {
        auto store = open_or_fail(dir);
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
        good_size = store->active_file_size();
    }

    append_raw(log_path(dir), make_raw_header(0xFFFFFFFFu, 0xFFFFFFFFu));

    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 1u);
    EXPECT_EQ(store->active_file_size(), good_size);
}

// --- Corruption caught on read ---------------------------------------------

TEST(BitcaskCorruption, FlippedValueByteIsCaughtOnGet) {
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_OK(store->put("alpha", "one"));

    // Corrupt while the store is open, so the in-memory index still points at
    // the record. Doing it after a close would instead exercise recovery, which
    // would truncate the record and answer NotFound -- a different code path.
    // Sole record, so it starts at offset 0: header, then "alpha", then "one".
    flip_byte_at(log_path(dir), record::kHeaderSize + 5);

    std::string value;
    EXPECT_STATUS(StatusCode::Corruption, store->get("alpha", &value));
}

TEST(BitcaskCorruption, FlippedHeaderByteIsCaughtOnGet) {
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_OK(store->put("alpha", "one"));

    flip_byte_at(log_path(dir), 4);  // First timestamp byte -- inside the crc's coverage.

    std::string value;
    EXPECT_STATUS(StatusCode::Corruption, store->get("alpha", &value));
}

TEST(BitcaskCorruption, HealthyKeysStillReadableAfterAnotherIsDamaged) {
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_OK(store->put("alpha", "one"));
    const std::uint64_t second_offset = store->active_file_size();
    EXPECT_OK(store->put("beta", "two"));

    flip_byte_at(log_path(dir), second_offset + record::kHeaderSize + 4);

    std::string value;
    EXPECT_STATUS(StatusCode::Corruption, store->get("beta", &value));
    // Damage is per-record: one bad record doesn't poison the rest.
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
}

// --- Durability mode -------------------------------------------------------

TEST(BitcaskDurability, BothSyncModesPersistAcrossAClose) {
    // A clean close flushes either way -- SyncMode is about surviving a *power*
    // cut, which a unit test cannot stage. This pins down that Never doesn't
    // break ordinary persistence.
    for (const SyncMode mode : {SyncMode::Always, SyncMode::Never}) {
        TempDir dir;
        {
            auto store = open_or_fail(dir, mode);
            ASSERT_NE(store, nullptr);
            EXPECT_OK(store->put("alpha", "one"));
        }

        auto store = open_or_fail(dir, mode);
        ASSERT_NE(store, nullptr);
        std::string value;
        EXPECT_OK(store->get("alpha", &value));
        EXPECT_EQ(value, "one");
    }
}

TEST(BitcaskDurability, ExplicitSyncSucceeds) {
    TempDir dir;
    auto store = open_or_fail(dir, SyncMode::Never);
    ASSERT_NE(store, nullptr);
    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->sync());
}

// --- Log layout ------------------------------------------------------------

TEST(BitcaskLog, AppendsGrowTheFileByExactlyTheRecordSize) {
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(store->active_file_size(), 0u);
    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_EQ(store->active_file_size(), record::kHeaderSize + 5 + 3);

    EXPECT_OK(store->put("beta", "twelve-chars"));
    EXPECT_EQ(store->active_file_size(), (record::kHeaderSize + 5 + 3) + (record::kHeaderSize + 4 + 12));
}

TEST(BitcaskLog, OverwriteAppendsRatherThanEditsInPlace) {
    // The core of the design: nothing on disk is ever modified, which is why a
    // crash can only ever damage the tail.
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_OK(store->put("alpha", "one"));
    const std::uint64_t after_first = store->active_file_size();
    EXPECT_OK(store->put("alpha", "two"));

    EXPECT_GT(store->active_file_size(), after_first);
    EXPECT_EQ(store->key_count(), 1u);  // Two records, one live key.
}

// --- Rotation --------------------------------------------------------------

TEST(BitcaskRotation, CrossingTheThresholdStartsANewFile) {
    TempDir dir;
    // Room for two of these records but not three.
    const std::uint64_t record_size = record::kHeaderSize + 3 + 5;
    auto store = open_or_fail(dir, tiny_files(record_size * 2 + 1));
    ASSERT_NE(store, nullptr);

    EXPECT_OK(store->put("aaa", "vvvvv"));
    EXPECT_OK(store->put("bbb", "vvvvv"));
    EXPECT_EQ(store->file_count(), 1u);
    const std::uint64_t sealed_size = store->active_file_size();

    EXPECT_OK(store->put("ccc", "vvvvv"));
    EXPECT_EQ(store->file_count(), 2u);
    EXPECT_EQ(store->active_file_id(), kFirstFileId + 1);

    // The third record went to the new file, and the sealed one stopped growing.
    EXPECT_EQ(store->active_file_size(), record_size);
    EXPECT_EQ(std::filesystem::file_size(data_path(dir, kFirstFileId)), sealed_size);
    EXPECT_EQ(data_file_ids(dir), (std::vector<FileId>{1, 2}));
}

TEST(BitcaskRotation, NoRecordStraddlesAFileBoundary) {
    // The framing has no way to continue a record into the next file, and the
    // index addresses a record by a single {file, offset} -- so the threshold
    // has to be checked before the append, never after.
    TempDir dir;
    constexpr std::uint64_t kThreshold = 200;
    auto store = open_or_fail(dir, tiny_files(kThreshold));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 60; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), std::string(i % 17, 'x')));
    }
    ASSERT_GT(store->file_count(), 3u);

    // Every file is either within the threshold, or holds exactly one record
    // that was too big for an empty file to satisfy.
    for (const FileId id : data_file_ids(dir)) {
        EXPECT_LE(std::filesystem::file_size(data_path(dir, id)), kThreshold)
            << "file " << id << " grew past the threshold";
    }
}

TEST(BitcaskRotation, ValuesWrittenBeforeRotationAreStillReadable) {
    // The point of threading file_id through ValuePointer: an offset alone
    // stopped being a unique address the moment there was a second file.
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(64));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 40; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    ASSERT_GT(store->file_count(), 1u);

    std::string value;
    for (int i = 0; i < 40; ++i) {
        EXPECT_OK(store->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

TEST(BitcaskRotation, ReopenReplaysEveryFileInIdOrder) {
    TempDir dir;
    std::size_t files_written = 0;
    {
        auto store = open_or_fail(dir, tiny_files(64));
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < 30; ++i) {
            // Same key over and over, so only the very last write is live. If
            // replay visited files in any order but ascending, this lands on
            // the wrong value.
            EXPECT_OK(store->put("counter", std::to_string(i)));
        }
        files_written = store->file_count();
        ASSERT_GT(files_written, 1u);
    }

    auto store = open_or_fail(dir, tiny_files(64));
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->file_count(), files_written);
    EXPECT_EQ(store->key_count(), 1u);

    std::string value;
    EXPECT_OK(store->get("counter", &value));
    EXPECT_EQ(value, "29");
}

TEST(BitcaskRotation, TheHighestIdBecomesActiveAndAcceptsWrites) {
    TempDir dir;
    FileId last_id = 0;
    {
        auto store = open_or_fail(dir, tiny_files(64));
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < 20; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), "value"));
        }
        last_id = store->active_file_id();
        ASSERT_GT(last_id, kFirstFileId);
    }

    {
        auto store = open_or_fail(dir, tiny_files(64));
        ASSERT_NE(store, nullptr);
        // Reused, not abandoned: reopening does not leave an empty file behind.
        EXPECT_EQ(store->active_file_id(), last_id);
        EXPECT_OK(store->put("after-reopen", "landed"));
    }

    auto store = open_or_fail(dir, tiny_files(64));
    ASSERT_NE(store, nullptr);
    std::string value;
    EXPECT_OK(store->get("after-reopen", &value));
    EXPECT_EQ(value, "landed");
}

TEST(BitcaskRotation, ARecordLargerThanTheThresholdGetsAFileToItself) {
    // An empty file never rotates, or an oversized record would rotate forever
    // and never be written.
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(32));
    ASSERT_NE(store, nullptr);

    const std::string big(500, 'x');
    EXPECT_OK(store->put("small", "s"));
    EXPECT_OK(store->put("big", big));
    EXPECT_OK(store->put("after", "a"));

    std::string value;
    EXPECT_OK(store->get("big", &value));
    EXPECT_EQ(value, big);
    EXPECT_OK(store->get("small", &value));
    EXPECT_EQ(value, "s");
    EXPECT_OK(store->get("after", &value));
    EXPECT_EQ(value, "a");

    // And it survives a reopen, which is where a straddled record would show up.
    store.reset();
    auto reopened = open_or_fail(dir, tiny_files(32));
    ASSERT_NE(reopened, nullptr);
    EXPECT_OK(reopened->get("big", &value));
    EXPECT_EQ(value, big);
    EXPECT_EQ(reopened->key_count(), 3u);
}

TEST(BitcaskRotation, ATornTailInTheActiveFileIsCutAndEarlierFilesAreLeftAlone) {
    TempDir dir;
    std::vector<FileId> ids;
    std::uint64_t sealed_size = 0;
    std::uint64_t active_size = 0;
    {
        auto store = open_or_fail(dir, tiny_files(64));
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < 20; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), "value"));
        }
        ids = data_file_ids(dir);
        ASSERT_GT(ids.size(), 2u);
        sealed_size = std::filesystem::file_size(data_path(dir, ids.front()));
        active_size = store->active_file_size();
    }

    // The crash lands in the file that was being appended to.
    append_raw(data_path(dir, ids.back()), {0x01, 0x02, 0x03, 0x04, 0x05, 0x06});

    auto store = open_or_fail(dir, tiny_files(64));
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->key_count(), 20u);
    EXPECT_EQ(std::filesystem::file_size(data_path(dir, ids.back())), active_size);
    // Recovery touches nothing but the tail of the active file.
    EXPECT_EQ(std::filesystem::file_size(data_path(dir, ids.front())), sealed_size);
}

TEST(BitcaskRotation, ABadRecordInASealedFileRefusesToOpen) {
    // A sealed file was written completely and fsynced before the engine moved
    // on, so a crash cannot explain a bad record in it -- damaged media can.
    // Truncating it, the way we truncate a torn tail, would destroy live data
    // in response to a read error, and do it silently.
    TempDir dir;
    std::vector<FileId> ids;
    {
        auto store = open_or_fail(dir, tiny_files(64));
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < 20; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), "value"));
        }
        ids = data_file_ids(dir);
        ASSERT_GT(ids.size(), 2u);
    }

    const std::uint64_t sealed_size = std::filesystem::file_size(data_path(dir, ids.front()));
    flip_byte_at(data_path(dir, ids.front()), record::kHeaderSize + 1);

    auto store = Bitcask::open(dir.path(), tiny_files(64));
    EXPECT_STATUS(StatusCode::Corruption, store.status());
    // And it said so instead of quietly repairing the file.
    EXPECT_EQ(std::filesystem::file_size(data_path(dir, ids.front())), sealed_size);
}

// --- Space accounting ------------------------------------------------------

TEST(BitcaskSpace, OverwritesAndDeletesAccumulateReclaimableBytes) {
    TempDir dir;
    auto store = open_or_fail(dir, SyncMode::Never);
    ASSERT_NE(store, nullptr);

    const std::uint64_t record_size = record::kHeaderSize + 5 + 3;
    const std::uint64_t tombstone_size = record::kHeaderSize + 5;

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_EQ(store->reclaimable_bytes(), 0u);  // Nothing superseded yet.

    EXPECT_OK(store->put("alpha", "two"));
    EXPECT_EQ(store->reclaimable_bytes(), record_size);  // The first record died.

    // A delete kills the record it buries *and* costs a tombstone, which is
    // itself dead weight -- deleting makes the file bigger before compaction
    // makes it smaller.
    EXPECT_OK(store->remove("alpha"));
    EXPECT_EQ(store->reclaimable_bytes(), record_size * 2 + tombstone_size);
    EXPECT_EQ(store->reclaimable_bytes(), store->total_disk_size());
}

TEST(BitcaskSpace, ReclaimableBytesSurviveReopen) {
    // Derived during replay from the same displacement events, so the number
    // has to come out identical rather than resetting to zero.
    TempDir dir;
    std::uint64_t before = 0;
    {
        auto store = open_or_fail(dir, tiny_files(64));
        ASSERT_NE(store, nullptr);
        for (int i = 0; i < 20; ++i) {
            EXPECT_OK(store->put("counter", std::to_string(i)));
        }
        EXPECT_OK(store->remove("gone"));
        before = store->reclaimable_bytes();
        ASSERT_GT(before, 0u);
    }

    auto store = open_or_fail(dir, tiny_files(64));
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->reclaimable_bytes(), before);
}

TEST(BitcaskLog, RemoveAppendsATombstone) {
    TempDir dir;
    auto store = open_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_OK(store->put("alpha", "one"));
    const std::uint64_t after_put = store->active_file_size();
    EXPECT_OK(store->remove("alpha"));

    // A tombstone is header + key, no value.
    EXPECT_EQ(store->active_file_size(), after_put + record::kHeaderSize + 5);
    EXPECT_EQ(store->key_count(), 0u);
}

}  // namespace
}  // namespace kvstore
