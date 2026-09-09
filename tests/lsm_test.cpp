#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "kvstore/lsm.hpp"
#include "kvstore/manifest.hpp"
#include "kvstore/sstable.hpp"
#include "kvstore_contract.hpp"
#include "lsm_helpers.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::collect;
using testing_support::keys_of;
using testing_support::open_lsm_or_fail;
using testing_support::sst_file_ids;
using testing_support::stacked_tables;
using testing_support::table_path;
using testing_support::TempDir;
using testing_support::temp_file_names;
using testing_support::tiny_lsm;
using testing_support::truncate_by;
using testing_support::wal_file_ids;
using testing_support::wal_path;

// --- The shared contract ---------------------------------------------------

struct LsmFactory {
    std::unique_ptr<KVStore> create() {
        LsmOptions options;
        options.sync_mode = SyncMode::Never;
        auto store = LsmStore::open(dir.path(), options);
        EXPECT_TRUE(store.is_ok()) << store.status().to_string();
        return store.is_ok() ? std::unique_ptr<KVStore>{store.take()} : nullptr;
    }

    TempDir dir;
};

INSTANTIATE_TYPED_TEST_SUITE_P(Lsm, KVStoreContract, LsmFactory);

// The same contract again, with a memtable threshold small enough that the
// cases cross it. This is not redundancy: it is the only way the shared suite
// exercises the *disk* path -- overwrites landing in a newer table than the
// value they replace, deletes shadowing tables below them, and ManyKeys reading
// back through several files rather than out of RAM.
struct FlushingLsmFactory {
    std::unique_ptr<KVStore> create() {
        auto store = LsmStore::open(dir.path(), tiny_lsm(/*memtable_size=*/512, /*block_size=*/64));
        EXPECT_TRUE(store.is_ok()) << store.status().to_string();
        return store.is_ok() ? std::unique_ptr<KVStore>{store.take()} : nullptr;
    }

    TempDir dir;
};

INSTANTIATE_TYPED_TEST_SUITE_P(FlushingLsm, KVStoreContract, FlushingLsmFactory);

// --- Layout on disk --------------------------------------------------------

TEST(LsmLayout, StartsWithOneEmptyLogAndNoTables) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(wal_file_ids(dir).size(), 1u);
    EXPECT_TRUE(sst_file_ids(dir).empty());
    EXPECT_EQ(store->table_count(), 0u);
}

TEST(LsmLayout, TableAndLogIdsComeFromOneCounter) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->flush());

    // The table took the id the retired log had; the fresh log took the next.
    const auto tables = sst_file_ids(dir);
    const auto logs = wal_file_ids(dir);
    ASSERT_EQ(tables.size(), 1u);
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_LT(tables.front(), logs.front())
        << "a higher id must always mean newer, across both kinds of file";
}

// --- Writes reach the memtable, reads come back out of it ------------------

TEST(LsmWrite, ReadsItsOwnWritesBeforeAnyFlush) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->put("beta", "two"));
    EXPECT_EQ(store->table_count(), 0u) << "nothing should have been flushed yet";

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
}

TEST(LsmWrite, EveryWriteIsLoggedBeforeItIsVisible) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    const auto path = wal_path(dir, wal_file_ids(dir).front());
    const auto before = std::filesystem::file_size(path);
    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_GT(std::filesystem::file_size(path), before)
        << "the write must be on disk before the memtable is asked about it";
}

TEST(LsmWrite, ADeleteIsAWriteAndIsLoggedToo) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "one"));

    const auto path = wal_path(dir, wal_file_ids(dir).front());
    const auto before = std::filesystem::file_size(path);
    EXPECT_OK(store->remove("alpha"));
    EXPECT_GT(std::filesystem::file_size(path), before);
}

// --- Flush -----------------------------------------------------------------

TEST(LsmFlush, TurnsTheMemtableIntoATableAndRetiresItsLog) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "one"));

    const FileId old_log = wal_file_ids(dir).front();
    EXPECT_OK(store->flush());

    EXPECT_EQ(store->table_count(), 1u);
    EXPECT_EQ(store->memtable_entries(), 0u);
    EXPECT_EQ(sst_file_ids(dir).size(), 1u);

    const auto logs = wal_file_ids(dir);
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_NE(logs.front(), old_log) << "the retired log must be gone, not reused";
    EXPECT_EQ(std::filesystem::file_size(wal_path(dir, logs.front())), 0u);

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one") << "the answer now has to come off disk";
}

TEST(LsmFlush, FlushingAnEmptyMemtableDoesNothing) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    const FileId log = wal_file_ids(dir).front();

    EXPECT_OK(store->flush());
    EXPECT_EQ(store->table_count(), 0u);
    EXPECT_TRUE(sst_file_ids(dir).empty());
    EXPECT_EQ(wal_file_ids(dir).front(), log);
}

// Written in scrambled order, read back sorted: the memtable did the ordering,
// so the flush is a single forward pass with no sorting step in it.
TEST(LsmFlush, ProducesASortedTableHoldingEveryKey) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, tiny_lsm(/*memtable_size=*/1u << 20, /*block_size=*/64));

    const std::vector<std::string> written = {"mike", "alpha", "zulu",  "delta", "bravo",
                                              "papa", "echo",  "sierra", "charlie"};
    for (const std::string& key : written) {
        EXPECT_OK(store->put(key, "v-" + key));
    }
    EXPECT_OK(store->flush());
    ASSERT_EQ(store->table_count(), 1u);

    const auto ids = sst_file_ids(dir);
    ASSERT_EQ(ids.size(), 1u);

    // Read the file ourselves rather than asking the engine, so this checks the
    // bytes on disk rather than the engine's memory of them.
    auto opened = SSTable::open(table_path(dir, ids.front()), ids.front());
    ASSERT_TRUE(opened.is_ok()) << opened.status().to_string();
    std::unique_ptr<SSTable> table = opened.take();

    EXPECT_EQ(table->entry_count(), written.size());

    std::vector<std::string> on_disk;
    for (std::size_t block = 0; block < table->block_count(); ++block) {
        auto payload = table->read_block(block);
        ASSERT_TRUE(payload.is_ok()) << payload.status().to_string();
        std::vector<sstable::BlockEntry> entries;
        ASSERT_TRUE(sstable::parse_block(payload->bytes(), &entries).is_ok());
        for (const auto& entry : entries) {
            on_disk.emplace_back(entry.key);
        }
    }

    std::vector<std::string> expected = written;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(on_disk, expected);
    EXPECT_EQ(table->min_key(), expected.front());
    EXPECT_EQ(table->max_key(), expected.back());
}

TEST(LsmFlush, CrossingTheThresholdFlushesWithoutBeingAsked) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, tiny_lsm(/*memtable_size=*/512, /*block_size=*/64));

    for (int i = 0; i < 200; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    // Flushes, not tables: compaction now merges the tables away behind us, so
    // counting files would be measuring compaction rather than the threshold.
    EXPECT_GT(store->stats().flushes.load(), 1u)
        << "the threshold should have been crossed repeatedly";
    EXPECT_LT(store->memtable_bytes(), 512u);

    for (int i = 0; i < 200; ++i) {
        std::string value;
        EXPECT_OK(store->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

// --- Tombstones across tables ----------------------------------------------

// The case the three-way Lookup exists for. Both keys live only on disk, in two
// different files, and the newer file's tombstone has to win.
TEST(LsmTombstones, ATombstoneInANewerTableShadowsAValueInAnOlderOne) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->put("beta", "two"));
    EXPECT_OK(store->flush());  // Table 1: alpha, beta.

    EXPECT_OK(store->remove("alpha"));
    EXPECT_OK(store->flush());  // Table 2: a tombstone for alpha.

    ASSERT_EQ(store->table_count(), 2u);
    ASSERT_EQ(store->memtable_entries(), 0u) << "the answer must come from disk, not RAM";

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("alpha", &value));
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
}

TEST(LsmTombstones, AValueWrittenAfterADeleteWinsOverTheTombstoneBelowIt) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->remove("alpha"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->put("alpha", "three"));
    EXPECT_OK(store->flush());

    ASSERT_EQ(store->table_count(), 3u);
    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "three");
}

TEST(LsmTombstones, ADeleteStillInTheMemtableShadowsATableBelowIt) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->remove("alpha"));  // Not flushed: still a memtable tombstone.

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("alpha", &value));
}

TEST(LsmTombstones, ANewerTablesValueWinsOverAnOlderOnes) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("alpha", "first"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->put("alpha", "second"));
    EXPECT_OK(store->flush());

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "second");
}

// --- Recovery --------------------------------------------------------------

TEST(LsmRecovery, AnUnflushedMemtableComesBackFromTheWriteAheadLog) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        EXPECT_OK(store->remove("alpha"));

        // Deliberately no flush. Everything written so far exists only in RAM
        // and in the log -- which is exactly the state a power cut leaves.
        ASSERT_TRUE(sst_file_ids(dir).empty());
        ASSERT_EQ(wal_file_ids(dir).size(), 1u);
    }

    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->stats().wal_records_replayed.load(), 3u);

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("alpha", &value));
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");

    // Recovery flushed what it replayed, so the state is the invariant one: a
    // table holding the recovered writes and exactly one empty log.
    EXPECT_EQ(sst_file_ids(dir).size(), 1u);
    ASSERT_EQ(wal_file_ids(dir).size(), 1u);
    EXPECT_EQ(std::filesystem::file_size(wal_path(dir, wal_file_ids(dir).front())), 0u);
}

TEST(LsmRecovery, FlushedTablesSurviveWithoutAnyLogToReplay) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        for (int i = 0; i < 50; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
        }
        EXPECT_OK(store->flush());
    }

    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->stats().wal_records_replayed.load(), 0u);
    EXPECT_EQ(store->table_count(), 1u);

    for (int i = 0; i < 50; ++i) {
        std::string value;
        EXPECT_OK(store->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

TEST(LsmRecovery, ReplaysTablesAndTheLogTogetherWithTheLogWinning) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        EXPECT_OK(store->put("alpha", "old"));
        EXPECT_OK(store->flush());
        EXPECT_OK(store->put("alpha", "new"));  // Only in the log.
    }

    auto store = open_lsm_or_fail(dir);
    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "new") << "the replayed write is newer than the table below it";
}

TEST(LsmRecovery, ATornWriteAtTheTailOfTheLogIsCutRatherThanRejected) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        EXPECT_OK(store->put("gamma", "three"));
    }

    const auto logs = wal_file_ids(dir);
    ASSERT_EQ(logs.size(), 1u);
    // Cut into the last record, the way a crash mid-append would. The write it
    // belongs to was never acknowledged, so losing it is correct.
    truncate_by(wal_path(dir, logs.front()), 4);

    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr) << "a torn tail must not stop the database from opening";

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");
    EXPECT_STATUS(StatusCode::NotFound, store->get("gamma", &value));
}

TEST(LsmRecovery, AnEmptyLogLeftBehindIsHarmless) {
    TempDir dir;
    { auto store = open_lsm_or_fail(dir); }
    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->table_count(), 0u);
    EXPECT_TRUE(sst_file_ids(dir).empty()) << "an empty memtable must not be flushed";
}

TEST(LsmRecovery, ManyReopensDoNotAccumulateFiles) {
    TempDir dir;
    for (int round = 0; round < 5; ++round) {
        auto store = open_lsm_or_fail(dir);
        EXPECT_OK(store->put("round", std::to_string(round)));
    }

    auto store = open_lsm_or_fail(dir);
    std::string value;
    EXPECT_OK(store->get("round", &value));
    EXPECT_EQ(value, "4");
    EXPECT_EQ(wal_file_ids(dir).size(), 1u) << "there must never be more than one live log";
}

// --- Crash points ----------------------------------------------------------

TEST(LsmCrash, ATableWrittenButNeverInstalledIsSweptAndTheLogReplays) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(
            dir, tiny_lsm(1u << 20, 64, LsmFailPoint::AfterWritingTable));
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->flush());

        EXPECT_TRUE(sst_file_ids(dir).empty()) << "nothing was installed";
        EXPECT_FALSE(temp_file_names(dir).empty()) << "the temp is still there";
        // Two logs: the one that fed the frozen memtable and was never retired,
        // and the one the rotation opened for the memtable that replaced it.
        // The background flush opens the new log *before* retiring the old, so
        // that a writer is never left with nowhere to log to.
        EXPECT_EQ(wal_file_ids(dir).size(), 2u) << "the old log was not retired";
    }

    auto store = open_lsm_or_fail(dir);
    EXPECT_TRUE(temp_file_names(dir).empty()) << "recovery must sweep the orphaned temp";

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
}

// A table can be renamed into place and still not be part of the database. The
// manifest decides what is real, so a crash between the rename and the commit
// leaves a file nothing references -- and recovery deletes it under the same one
// rule that cleans up after a compaction.
//
// This is where the commit point moved in Step 2. Under Step 1 the rename *was*
// the commit, and recovery kept this table; now it does not, which is strictly
// tidier and needs no second rule.
TEST(LsmCrash, ATableRenamedButNeverCommittedIsSweptAsAnOrphan) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(
            dir, tiny_lsm(1u << 20, 64, LsmFailPoint::AfterInstallingTable));
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        EXPECT_OK(store->flush());

        EXPECT_EQ(sst_file_ids(dir).size(), 1u) << "the file exists under its real name";
        EXPECT_EQ(store->table_count(), 0u) << "and the engine does not count it";
        EXPECT_EQ(wal_file_ids(dir).size(), 2u) << "the old log was not retired";
    }

    auto store = open_lsm_or_fail(dir);
    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");

    // The orphan went, and the replayed log produced the one real table.
    EXPECT_EQ(sst_file_ids(dir).size(), 1u);
    EXPECT_EQ(store->table_count(), 1u);
    EXPECT_GT(store->stats().tables_obsoleted.load(), 0u) << "the orphan was swept";
}

// The window that genuinely does leave duplicate data: the manifest is
// committed, so the table is real, but the log that fed it has not been retired
// yet. Recovery replays that log into a second, identical table -- wasted space,
// not wrong data, because the newer one shadows the older key for key.
//
// This is flush idempotence, and it is what makes the window safe to *have*
// rather than something that had to be eliminated.
TEST(LsmCrash, ATableCommittedBeforeItsLogWasRetiredRecoversToTheSameContents) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(
            dir, tiny_lsm(1u << 20, 64, LsmFailPoint::AfterManifestCommit));
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->put("beta", "two"));
        EXPECT_OK(store->flush());

        EXPECT_EQ(store->table_count(), 1u) << "committed, so it is real";
        EXPECT_EQ(wal_file_ids(dir).size(), 2u) << "table and log hold the same writes at once";
    }

    auto store = open_lsm_or_fail(dir);
    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_OK(store->get("beta", &value));
    EXPECT_EQ(value, "two");

    EXPECT_EQ(store->table_count(), 2u)
        << "the replay produced a duplicate table, which the newer one shadows";
}

// --- The manifest, from the engine's side ----------------------------------

TEST(LsmManifest, IsWrittenOnTheFirstOpenAndNamesEveryTable) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    ASSERT_TRUE(std::filesystem::exists(dir.path() / "MANIFEST"));

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->flush());

    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok()) << set.status().to_string();
    ASSERT_TRUE(set->has_value());
    EXPECT_EQ((*set)->table_count(), 1u);
    EXPECT_EQ((*set)->levels[0][0].min_key, "alpha");
    EXPECT_EQ((*set)->levels[0][0].max_key, "alpha");
    EXPECT_GT((*set)->next_id, (*set)->levels[0][0].id)
        << "next_id must be past every table it names";
}

// The compatibility path: a directory written before manifests existed still
// opens, and gains one. Simulated by deleting the manifest, which leaves exactly
// the layout Step 1 produced.
TEST(LsmManifest, ADirectoryWithoutOneIsAdoptedRatherThanEmptied) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        for (int i = 0; i < 3; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
            EXPECT_OK(store->flush());
        }
        ASSERT_EQ(store->table_count(), 3u);
    }
    ASSERT_EQ(sst_file_ids(dir).size(), 3u);
    std::filesystem::remove(dir.path() / "MANIFEST");

    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->table_count(), 3u) << "the tables must be adopted, not swept";
    EXPECT_EQ(store->tables_at(0), 3u) << "all of them at L0, which is what they were";
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "MANIFEST")) << "and one is written";

    for (int i = 0; i < 3; ++i) {
        std::string value;
        EXPECT_OK(store->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

TEST(LsmManifest, ADamagedManifestStopsTheDatabaseOpeningRatherThanLosingTables) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->flush());
    }
    testing_support::flip_byte_at(dir.path() / "MANIFEST", 20);

    auto store = LsmStore::open(dir.path(), LsmOptions{SyncMode::Never});
    EXPECT_STATUS(StatusCode::Corruption, store.status());
    EXPECT_EQ(sst_file_ids(dir).size(), 1u)
        << "and it must not have deleted the tables on its way out";
}

TEST(LsmManifest, AnSstNotNamedByTheManifestIsSweptAtOpen) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir);
        EXPECT_OK(store->put("alpha", "one"));
        EXPECT_OK(store->flush());
    }
    // A plausible-looking table nothing references, exactly as an interrupted
    // compaction would have left behind.
    testing_support::append_raw(dir.path() / sst_file_name(999),
                                std::vector<std::uint8_t>(200, 0xCD));
    ASSERT_EQ(sst_file_ids(dir).size(), 2u);

    auto store = open_lsm_or_fail(dir);
    ASSERT_NE(store, nullptr) << "an orphan must not stop the database opening";
    EXPECT_EQ(sst_file_ids(dir).size(), 1u);
    EXPECT_EQ(store->table_count(), 1u);
}

// --- Scan: the second Bitcask limitation, lifted ---------------------------

TEST(LsmScan, ReturnsEveryLiveKeyInOrderAcrossMemtableAndTables) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("bravo", "2"));
    EXPECT_OK(store->put("delta", "4"));
    EXPECT_OK(store->flush());  // Oldest table.
    EXPECT_OK(store->put("alpha", "1"));
    EXPECT_OK(store->put("charlie", "3"));
    EXPECT_OK(store->flush());  // Newer table.
    EXPECT_OK(store->put("echo", "5"));  // Still in the memtable.

    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok()) << opened.status().to_string();
    Iterator cursor = opened.take();

    const auto rows = collect(cursor);
    EXPECT_OK(cursor.status());
    EXPECT_EQ(keys_of(rows),
              (std::vector<std::string>{"alpha", "bravo", "charlie", "delta", "echo"}));
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].second, "1");
    EXPECT_EQ(rows[4].second, "5");
}

TEST(LsmScan, HonoursTheHalfOpenRange) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    for (const char* key : {"alpha", "bravo", "charlie", "delta", "echo"}) {
        EXPECT_OK(store->put(key, key));
    }
    EXPECT_OK(store->flush());

    auto opened = store->scan("bravo", "delta");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();
    EXPECT_EQ(keys_of(collect(cursor)), (std::vector<std::string>{"bravo", "charlie"}))
        << "begin is inclusive, end is exclusive";
}

TEST(LsmScan, ARangeThatMatchesNothingIsEmptyRatherThanAnError) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "1"));
    EXPECT_OK(store->flush());

    auto opened = store->scan("xray", "zulu");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();
    EXPECT_FALSE(cursor.valid());
    EXPECT_OK(cursor.status());
}

TEST(LsmScan, SkipsDeletedKeysEntirely) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "1"));
    EXPECT_OK(store->put("bravo", "2"));
    EXPECT_OK(store->put("charlie", "3"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->remove("bravo"));
    EXPECT_OK(store->flush());

    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();
    // A deleted key is not a row with no value; it is not a row.
    EXPECT_EQ(keys_of(collect(cursor)), (std::vector<std::string>{"alpha", "charlie"}));
}

TEST(LsmScan, ReturnsTheNewestValueForAKeyThatLivesInSeveralTables) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "first"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->put("alpha", "second"));
    EXPECT_OK(store->flush());
    EXPECT_OK(store->put("alpha", "third"));

    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();

    const auto rows = collect(cursor);
    ASSERT_EQ(rows.size(), 1u) << "one key, however many copies of it are on disk";
    EXPECT_EQ(rows[0].second, "third");
}

TEST(LsmScan, WalksAcrossManyBlocksAndManyTablesInOrder) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, tiny_lsm(/*memtable_size=*/512, /*block_size=*/64));

    constexpr int kCount = 300;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(store->put("key" + std::string(3 - std::to_string(i).size(), '0') +
                                 std::to_string(i),
                             "value" + std::to_string(i)));
    }
    ASSERT_GT(store->table_count(), 1u) << "the test needs several tables to be meaningful";

    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();

    const auto rows = collect(cursor);
    EXPECT_OK(cursor.status());
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kCount));
    for (std::size_t i = 1; i < rows.size(); ++i) {
        EXPECT_LT(rows[i - 1].first, rows[i].first) << "the merge must produce sorted output";
    }
}

TEST(LsmScan, SurvivesAReopenAndStillSeesEverything) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir, tiny_lsm(512, 64));
        for (int i = 0; i < 100; ++i) {
            EXPECT_OK(store->put("key" + std::to_string(i), "v"));
        }
        EXPECT_OK(store->remove("key42"));
    }

    auto store = open_lsm_or_fail(dir);
    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();

    const auto rows = collect(cursor);
    EXPECT_EQ(rows.size(), 99u) << "one of the hundred was deleted before the reopen";
}

// --- Statistics ------------------------------------------------------------

TEST(LsmStatistics, CountAGetAgainstTheTablesItActuallyTouched) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->flush());

    const std::uint64_t before = store->stats().block_reads.load();
    std::string value;
    EXPECT_OK(store->get("alpha", &value));

    EXPECT_EQ(store->stats().block_reads.load(), before + 1)
        << "one key, one table, one block";
    EXPECT_GE(store->stats().tables_probed.load(), 1u);
}

// Read amplification, made visible: a key in the newest table stops the search
// immediately, while an absent one has to ask every table before giving up.
TEST(LsmStatistics, AnAbsentKeyCostsMoreTablesThanAPresentOne) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, stacked_tables());
    for (int i = 0; i < 4; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "v"));
        EXPECT_OK(store->flush());
    }
    ASSERT_EQ(store->table_count(), 4u);

    std::string value;
    const std::uint64_t before_present = store->stats().tables_probed.load();
    EXPECT_OK(store->get("key3", &value));  // Newest table.
    const std::uint64_t present_cost = store->stats().tables_probed.load() - before_present;

    const std::uint64_t before_absent = store->stats().tables_probed.load();
    EXPECT_STATUS(StatusCode::NotFound, store->get("nothere", &value));
    const std::uint64_t absent_cost = store->stats().tables_probed.load() - before_absent;

    EXPECT_EQ(present_cost, 1u);
    EXPECT_EQ(absent_cost, 4u) << "this is the number Step 2's bloom filters are aimed at";
}

// --- Bloom filters ---------------------------------------------------------

// The headline number for this phase. Four tables, an absent key that falls
// *inside* every one of their key ranges -- so the cheap min/max check cannot
// answer it -- and not one block comes off the disk.
TEST(LsmBloom, AnAbsentKeyInsideEveryTablesRangeCostsNoBlockReadsAtAll) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, stacked_tables());

    for (int t = 0; t < 4; ++t) {
        // "aaa" and "zzz" pin every table's range wide open, so the range check
        // is useless and only the filter can reject anything.
        EXPECT_OK(store->put("aaa", "low"));
        EXPECT_OK(store->put("zzz", "high"));
        for (int i = 0; i < 50; ++i) {
            EXPECT_OK(store->put("t" + std::to_string(t) + "key" + std::to_string(i), "v"));
        }
        EXPECT_OK(store->flush());
    }
    ASSERT_EQ(store->table_count(), 4u);

    const std::uint64_t blocks_before = store->stats().block_reads.load();
    const std::uint64_t rejects_before = store->stats().bloom_rejects.load();
    const std::uint64_t probes_before = store->stats().tables_probed.load();

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("mmm-definitely-absent", &value));

    EXPECT_EQ(store->stats().block_reads.load(), blocks_before)
        << "the filters answered entirely from RAM";
    EXPECT_EQ(store->stats().bloom_rejects.load(), rejects_before + 4)
        << "every one of the four tables rejected it";
    // All four tables are still *asked* -- filters do not reduce that, and Step 1
    // measured it at four. What changed is that asking now costs no disk.
    EXPECT_EQ(store->stats().tables_probed.load(), probes_before + 4);
}

// The subtlest correctness point in the phase. If a tombstone's key were left
// out of the filter, the newer table would deny all knowledge of the deleted
// key, the search would fall through, and the older table would hand back the
// value it still holds. The delete would silently undo itself.
//
// The newer table carries 200 other keys so its filter is properly sized -- with
// only one key the filter would be 64 bits and a false positive could mask the
// bug entirely.
TEST(LsmBloom, ADeletedKeysTombstoneIsInTheFilterSoItStillShadowsOlderTables) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);

    EXPECT_OK(store->put("victim", "original"));
    EXPECT_OK(store->flush());  // Older table: victim -> "original".

    EXPECT_OK(store->remove("victim"));
    for (int i = 0; i < 200; ++i) {
        EXPECT_OK(store->put("filler" + std::to_string(i), "v"));
    }
    EXPECT_OK(store->flush());  // Newer table: a tombstone among 200 live keys.

    ASSERT_EQ(store->table_count(), 2u);
    ASSERT_EQ(store->memtable_entries(), 0u) << "the answer must come off disk";

    // If the tombstone's key were missing from the newer table's filter, this
    // would come back Ok with "original".
    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("victim", &value));
}

TEST(LsmBloom, APresentKeyStillCostsExactlyOneBlockRead) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir);
    for (int i = 0; i < 100; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "v"));
    }
    EXPECT_OK(store->flush());

    const std::uint64_t before = store->stats().block_reads.load();
    std::string value;
    EXPECT_OK(store->get("key50", &value));
    EXPECT_EQ(store->stats().block_reads.load(), before + 1)
        << "a filter must not cost the present case anything";
}

TEST(LsmBloom, TheFilterIsConsultedOncePerTableAsked) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, stacked_tables());
    for (int t = 0; t < 3; ++t) {
        EXPECT_OK(store->put("aaa", "low"));
        EXPECT_OK(store->put("zzz", "high"));
        EXPECT_OK(store->flush());
    }

    const std::uint64_t before = store->stats().bloom_checks.load();
    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("mmm", &value));
    EXPECT_EQ(store->stats().bloom_checks.load(), before + 3);
}

// A filter is allowed to be wrong in one direction only. Whatever it says, the
// answers must be identical -- the filter is an optimisation, and an
// optimisation that changes results is a bug.
TEST(LsmBloom, ResultsAreIdenticalWithManyTablesAndManyAbsentKeys) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, tiny_lsm(/*memtable_size=*/512, /*block_size=*/64));

    constexpr int kCount = 400;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    ASSERT_GT(store->table_count(), 1u);

    for (int i = 0; i < kCount; ++i) {
        std::string value;
        EXPECT_OK(store->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
    for (int i = kCount; i < kCount * 2; ++i) {
        std::string value;
        EXPECT_STATUS(StatusCode::NotFound, store->get("key" + std::to_string(i), &value));
    }
}

}  // namespace
}  // namespace kvstore
