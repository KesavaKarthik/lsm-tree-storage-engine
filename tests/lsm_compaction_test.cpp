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
using testing_support::leveled;
using testing_support::open_lsm_or_fail;
using testing_support::sst_file_ids;
using testing_support::TempDir;
using testing_support::temp_file_names;

// Zero-padded so lexicographic order and numeric order agree.
std::string key_at(int i) {
    std::string digits = std::to_string(i);
    return "key" + std::string(4 - digits.size(), '0') + digits;
}

// The invariant every level below L0 must hold, checked against the manifest --
// which is both the thing recovery will trust and the thing the compaction
// picker reads to compute overlaps.
void expect_levels_disjoint(const TempDir& dir) {
    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok()) << set.status().to_string();
    ASSERT_TRUE(set->has_value());

    for (std::size_t level = 1; level < (*set)->levels.size(); ++level) {
        const auto& tables = (*set)->levels[level];
        for (std::size_t i = 1; i < tables.size(); ++i) {
            EXPECT_LT(tables[i - 1].max_key, tables[i].min_key)
                << "level " << level << " tables " << (i - 1) << " and " << i << " overlap";
        }
    }
}

void fill(LsmStore& store, int from, int to, const std::string& tag = "v") {
    for (int i = from; i < to; ++i) {
        ASSERT_TRUE(store.put(key_at(i), tag + std::to_string(i)).is_ok());
    }
}

void expect_all_present(LsmStore& store, int from, int to, const std::string& tag = "v") {
    for (int i = from; i < to; ++i) {
        std::string value;
        SCOPED_TRACE(key_at(i));
        EXPECT_OK(store.get(key_at(i), &value));
        EXPECT_EQ(value, tag + std::to_string(i));
    }
}

// --- It happens, and it happens on its own -------------------------------

TEST(LsmCompaction, TheL0TriggerFiresWithoutBeingAsked) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    fill(*store, 0, 200);
    EXPECT_OK(store->flush());

    EXPECT_GT(store->stats().compactions.load(), 0u)
        << "a bounded step should have run after a flush";
    EXPECT_LT(store->tables_at(0), 3u) << "L0 must not be allowed to pile up";
    EXPECT_GT(store->level_count(), 1u) << "data should have moved down a level";
    expect_all_present(*store, 0, 200);
}

TEST(LsmCompaction, MovesDataOutOfLevelZeroIntoLevelOne) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    fill(*store, 0, 100);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    // compact() is the unbounded form: it drives everything down until one
    // level holds the data, so L0 ends up empty.
    EXPECT_EQ(store->tables_at(0), 0u);
    EXPECT_GT(store->table_count(), 0u);
    expect_all_present(*store, 0, 100);
}

TEST(LsmCompaction, LevelsBelowZeroHoldDisjointRanges) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    fill(*store, 0, 400);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    ASSERT_GT(store->level_count(), 1u);
    expect_levels_disjoint(dir);
    expect_all_present(*store, 0, 400);
}

TEST(LsmCompaction, SplitsItsOutputIntoBoundedTables) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled(/*memtable_size=*/256, /*l0_trigger=*/2,
                                               /*level_base=*/1u << 20,
                                               /*target_table=*/512));
    fill(*store, 0, 400);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    EXPECT_GT(store->tables_at(1), 1u) << "one merge should yield several bounded files";
    expect_levels_disjoint(dir);
}

// --- Space ----------------------------------------------------------------

// The point of compaction, measured: rewriting the same keys over and over
// leaves the disk holding one copy, not ten.
TEST(LsmCompaction, ReclaimsTheSpaceHeldBySupersededValues) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    for (int round = 0; round < 10; ++round) {
        fill(*store, 0, 100, "round" + std::to_string(round) + "-");
        EXPECT_OK(store->flush());
    }
    const std::uint64_t before = store->total_disk_size();
    EXPECT_OK(store->compact());
    const std::uint64_t after = store->total_disk_size();

    EXPECT_LT(after, before) << "ten copies of a hundred keys became one";
    expect_all_present(*store, 0, 100, "round9-");
}

// A tombstone may be discarded only once nothing below can still hold the value
// it buries. When everything is deleted and there is no level below, the whole
// tree should reclaim to nothing.
TEST(LsmCompaction, TombstonesAreDroppedOnceNothingBelowCanHoldTheKey) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    fill(*store, 0, 100);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());
    ASSERT_GT(store->table_count(), 0u);

    for (int i = 0; i < 100; ++i) {
        EXPECT_OK(store->remove(key_at(i)));
    }
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    EXPECT_EQ(store->table_count(), 0u)
        << "values gone, and the tombstones that buried them gone too";

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get(key_at(0), &value));
    EXPECT_STATUS(StatusCode::NotFound, store->get(key_at(99), &value));
}

// The failure a premature tombstone drop causes is a delete undoing itself, and
// it would show up only after the *next* compaction moved the tombstone past the
// value it was shadowing. So: delete, compact hard, write more, compact again,
// and check the deleted keys stayed dead the whole way through.
TEST(LsmCompaction, DeletedKeysNeverComeBackHoweverMuchCompactionRuns) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    fill(*store, 0, 300);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    for (int i = 0; i < 300; i += 3) {
        EXPECT_OK(store->remove(key_at(i)));
    }
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    const auto check = [&]() {
        for (int i = 0; i < 300; ++i) {
            std::string value;
            if (i % 3 == 0) {
                EXPECT_STATUS(StatusCode::NotFound, store->get(key_at(i), &value));
            } else {
                SCOPED_TRACE(key_at(i));
                EXPECT_OK(store->get(key_at(i), &value));
                EXPECT_EQ(value, "v" + std::to_string(i));
            }
        }
    };
    check();

    // More data, pushing the survivors down another level, then again.
    fill(*store, 300, 600);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());
    check();
    expect_all_present(*store, 300, 600);
}

// --- Everything still reads -----------------------------------------------

TEST(LsmCompaction, ScanStaysOrderedAndCompleteAcrossLevels) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    fill(*store, 0, 300);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());
    ASSERT_GT(store->level_count(), 1u);

    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok()) << opened.status().to_string();
    Iterator cursor = opened.take();

    const auto rows = collect(cursor);
    EXPECT_OK(cursor.status());
    ASSERT_EQ(rows.size(), 300u);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        EXPECT_LT(rows[i - 1].first, rows[i].first);
    }
    EXPECT_EQ(rows.front().first, key_at(0));
    EXPECT_EQ(rows.back().first, key_at(299));
}

TEST(LsmCompaction, ARangeScanStillWorksAfterTheDataHasMovedDown) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());
    fill(*store, 0, 300);
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    auto opened = store->scan(key_at(100), key_at(110));
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();

    std::vector<std::string> expected;
    for (int i = 100; i < 110; ++i) {
        expected.push_back(key_at(i));
    }
    EXPECT_EQ(keys_of(collect(cursor)), expected);
}

TEST(LsmCompaction, ContentsSurviveAReopenAfterCompaction) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir, leveled());
        fill(*store, 0, 300);
        EXPECT_OK(store->flush());
        EXPECT_OK(store->compact());
    }

    auto store = open_lsm_or_fail(dir, leveled());
    ASSERT_NE(store, nullptr);
    expect_all_present(*store, 0, 300);
    expect_levels_disjoint(dir);
}

// --- Crashes ---------------------------------------------------------------

// The outputs were written but never renamed. Nothing references the temps, and
// the inputs are untouched, so the tree is exactly as it was.
TEST(LsmCompactionCrash, OutputsWrittenButNeverInstalledLeaveTheTreeUnchanged) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir, leveled());
        fill(*store, 0, 200);
        EXPECT_OK(store->flush());
        EXPECT_OK(store->compact());
    }

    const std::vector<FileId> before = sst_file_ids(dir);
    ASSERT_FALSE(before.empty());

    {
        auto store = open_lsm_or_fail(
            dir, leveled(256, 2, 2048, 1024, LsmFailPoint::AfterWritingTable));
        fill(*store, 200, 400);
        EXPECT_OK(store->flush());
        EXPECT_FALSE(temp_file_names(dir).empty()) << "a temp should be sitting there";
    }

    auto store = open_lsm_or_fail(dir, leveled());
    EXPECT_TRUE(temp_file_names(dir).empty()) << "recovery sweeps it";
    expect_all_present(*store, 0, 400);
    expect_levels_disjoint(dir);
}

// The outputs were renamed but the manifest never committed. They are orphans --
// the manifest is the only thing that makes a table real -- and the inputs are
// still named, so the old tree is intact.
TEST(LsmCompactionCrash, OutputsRenamedButNeverCommittedAreSweptAndTheInputsSurvive) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir, leveled());
        fill(*store, 0, 200);
        EXPECT_OK(store->flush());
        EXPECT_OK(store->compact());
    }

    {
        auto store = open_lsm_or_fail(
            dir, leveled(256, 2, 2048, 1024, LsmFailPoint::AfterInstallingTable));
        fill(*store, 200, 400);
        EXPECT_OK(store->flush());
    }

    auto store = open_lsm_or_fail(dir, leveled());
    ASSERT_NE(store, nullptr);
    EXPECT_GT(store->stats().tables_obsoleted.load(), 0u) << "orphans were swept";
    expect_all_present(*store, 0, 400);
    expect_levels_disjoint(dir);
}

// The manifest committed but the inputs were never unlinked. They are now the
// orphans, and the same one rule removes them.
TEST(LsmCompactionCrash, InputsLeftBehindAfterTheCommitAreSweptAndNothingIsDuplicated) {
    TempDir dir;
    {
        auto store = open_lsm_or_fail(dir, leveled());
        fill(*store, 0, 200);
        EXPECT_OK(store->flush());
        EXPECT_OK(store->compact());
    }

    {
        auto store = open_lsm_or_fail(
            dir, leveled(256, 2, 2048, 1024, LsmFailPoint::AfterManifestCommit));
        fill(*store, 200, 400);
        EXPECT_OK(store->flush());
    }

    auto store = open_lsm_or_fail(dir, leveled());
    ASSERT_NE(store, nullptr);
    expect_all_present(*store, 0, 400);
    expect_levels_disjoint(dir);

    // Every file on disk is one the manifest names -- no strays either way.
    // Waiting first, because a compaction retiring files is what would create a
    // discrepancy, and it runs on another thread.
    EXPECT_OK(store->wait_for_background());
    auto set = manifest::read(dir.path());
    ASSERT_TRUE(set.is_ok());
    ASSERT_TRUE(set->has_value());
    EXPECT_EQ(sst_file_ids(dir).size(), (*set)->table_count());
}

TEST(LsmCompactionCrash, RepeatedCrashesAtEveryStageStillConverge) {
    TempDir dir;
    const LsmFailPoint points[] = {LsmFailPoint::AfterWritingTable,
                                   LsmFailPoint::AfterInstallingTable,
                                   LsmFailPoint::AfterManifestCommit};

    int written = 0;
    for (const LsmFailPoint point : points) {
        auto store = open_lsm_or_fail(dir, leveled(256, 2, 2048, 1024, point));
        ASSERT_NE(store, nullptr);
        fill(*store, written, written + 100);
        written += 100;
        EXPECT_OK(store->flush());
    }

    auto store = open_lsm_or_fail(dir, leveled());
    ASSERT_NE(store, nullptr);
    EXPECT_OK(store->compact());
    expect_all_present(*store, 0, written);
    expect_levels_disjoint(dir);
    EXPECT_TRUE(temp_file_names(dir).empty());
}

// --- Bloom filters survive compaction --------------------------------------

// Compaction writes new tables, so it also writes new filters. A merged table
// whose filter had lost the tombstones it carries would resurrect deleted keys.
TEST(LsmCompaction, MergedTablesCarryWorkingFilters) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, leveled());

    // Only the even keys, so the odd ones are absent but fall *inside* the
    // table's key range -- otherwise the min/max check answers first and the
    // filter is never consulted, which is correct behaviour and a useless test.
    for (int i = 0; i < 600; i += 2) {
        EXPECT_OK(store->put(key_at(i), "v" + std::to_string(i)));
    }
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());

    const std::uint64_t blocks_before = store->stats().block_reads.load();
    const std::uint64_t rejects_before = store->stats().bloom_rejects.load();
    const std::uint64_t checks_before = store->stats().bloom_checks.load();

    std::string value;
    for (int i = 1; i < 200; i += 2) {
        EXPECT_STATUS(StatusCode::NotFound, store->get(key_at(i), &value));
    }

    EXPECT_GT(store->stats().bloom_checks.load(), checks_before)
        << "the merged tables must have filters at all";
    EXPECT_GT(store->stats().bloom_rejects.load(), rejects_before)
        << "and those filters must be rejecting keys that are not there";
    // A hundred absent lookups inside the range. A few may cost a block to a
    // false positive; a hundred would mean compaction had dropped the filters.
    EXPECT_LT(store->stats().block_reads.load() - blocks_before, 20u);

    // And the keys that are there are still there.
    for (int i = 0; i < 600; i += 2) {
        SCOPED_TRACE(key_at(i));
        EXPECT_OK(store->get(key_at(i), &value));
        EXPECT_EQ(value, "v" + std::to_string(i));
    }
}


// A compaction step that installs nothing has changed nothing, so the next pick
// chooses the same work again. Before this was guarded, compact() spun its full
// ten-thousand-step safety limit and left ten thousand temp files behind.
//
// A fail point is the reachable way to produce a step that installs nothing --
// the process is supposed to have died inside it. The loop must stop there, for
// the same reason recover() must not run on past one.
TEST(LsmCompaction, CompactStopsWhenAStepMakesNoProgress) {
    TempDir dir;
    // Data at more than one level, so a forced pick genuinely has work to find.
    // (Leaving everything in a single level makes this test pass vacuously.)
    {
        auto store = open_lsm_or_fail(dir, leveled());
        fill(*store, 0, 400);
        EXPECT_OK(store->flush());
        ASSERT_GT(store->level_count(), 1u);
    }

    // Both fail points that stop *before* the manifest commit install nothing.
    // (AfterManifestCommit does install, so the loop is right to continue past
    // it -- it has made real progress.)
    for (const LsmFailPoint point :
         {LsmFailPoint::AfterWritingTable, LsmFailPoint::AfterInstallingTable}) {
        auto store = open_lsm_or_fail(dir, leveled(256, 2, 2048, 1024, point));
        ASSERT_NE(store, nullptr);
        ASSERT_GT(store->level_count(), 1u) << "the tree must still span levels";

        const std::size_t temps_before = temp_file_names(dir).size();
        EXPECT_OK(store->compact());

        // One step's worth of output, not ten thousand.
        EXPECT_LT(temp_file_names(dir).size() - temps_before, 20u);
    }
}

// The shallowest level is the only one whose tables may overlap, so the order
// they are consulted in *is* the correctness rule: newest first. That order has
// to survive being written to the manifest and read back.
//
// Nothing else tests this, and it would fail silently -- a lookup would simply
// start returning a stale value. It is exactly what a future change that sorted
// every level by key range would break.
TEST(LsmManifestOrdering, LevelZeroStaysNewestFirstAcrossAReopen) {
    TempDir dir;
    {
        // Compaction off: this needs several overlapping L0 tables to sit still.
        auto store = open_lsm_or_fail(dir, testing_support::stacked_tables());
        for (int version = 1; version <= 4; ++version) {
            EXPECT_OK(store->put("contested", "version" + std::to_string(version)));
            EXPECT_OK(store->put("witness" + std::to_string(version), "present"));
            EXPECT_OK(store->flush());
        }
        ASSERT_EQ(store->tables_at(0), 4u) << "four overlapping tables, all holding the key";

        std::string value;
        EXPECT_OK(store->get("contested", &value));
        ASSERT_EQ(value, "version4");
    }

    auto store = open_lsm_or_fail(dir, testing_support::stacked_tables());
    ASSERT_NE(store, nullptr);
    ASSERT_EQ(store->tables_at(0), 4u);

    // Every table holds a value for this key. Only the ordering decides which
    // one is returned, so a reordered level shows up here and nowhere else.
    std::string value;
    EXPECT_OK(store->get("contested", &value));
    EXPECT_EQ(value, "version4") << "level 0 must still be newest-first after a reopen";

    // The ids themselves, descending: the invariant stated directly rather than
    // inferred from one lookup.
    const std::vector<FileId> ids = store->table_ids_at(0);
    ASSERT_EQ(ids.size(), 4u);
    for (std::size_t i = 1; i < ids.size(); ++i) {
        EXPECT_LT(ids[i], ids[i - 1]) << "level 0 must be ordered newest id first";
    }

    // And a scan agrees, since the merge depends on the same ordering.
    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok());
    Iterator cursor = opened.take();
    const auto rows = collect(cursor);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().first, "contested");
    EXPECT_EQ(rows.front().second, "version4");
}

}  // namespace
}  // namespace kvstore
