#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/file_names.hpp"
#include "kvstore/record.hpp"
#include "kvstore_contract.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::bytes_on_disk;
using testing_support::data_file_ids;
using testing_support::data_path;
using testing_support::open_or_fail;
using testing_support::TempDir;
using testing_support::temp_file_names;
using testing_support::tiny_files;

// Small files, so a handful of short writes produces several of them.
constexpr std::uint64_t kSmall = 80;

// Asserts the whole database reads back as `expected`, and that nothing else
// is live. This is the "is it still correct" half of every crash test.
void expect_contents(Bitcask& store,
                     const std::vector<std::pair<std::string, std::string>>& expected) {
    EXPECT_EQ(store.key_count(), expected.size());
    std::string value;
    for (const auto& [key, want] : expected) {
        SCOPED_TRACE("reading " + key);
        EXPECT_OK(store.get(key, &value));
        EXPECT_EQ(value, want);
    }
}

// --- Reclaiming space ------------------------------------------------------

TEST(Compaction, ReclaimsTheSpaceHeldByOverwrittenValues) {
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    // One key, written a hundred times. Ninety-nine of those records are dead
    // the instant the next one lands -- this is space amplification, and it is
    // what an append-only store does to itself with no compaction.
    for (int i = 0; i < 100; ++i) {
        EXPECT_OK(store->put("counter", "value-" + std::to_string(i)));
    }

    const std::uint64_t before = bytes_on_disk(dir);
    const std::uint64_t dead = store->reclaimable_bytes();
    ASSERT_GT(before, 0u);
    ASSERT_GT(dead, 0u);
    ASSERT_GT(store->file_count(), 5u);

    EXPECT_OK(store->compact());

    const std::uint64_t after = bytes_on_disk(dir);
    EXPECT_LT(after, before);
    // Exactly the dead bytes came back -- the accounting is not an estimate.
    EXPECT_EQ(after, before - dead);
    EXPECT_EQ(store->reclaimable_bytes(), 0u);

    // And the surviving value is the newest one.
    std::string value;
    EXPECT_OK(store->get("counter", &value));
    EXPECT_EQ(value, "value-99");
    EXPECT_EQ(store->key_count(), 1u);
}

TEST(Compaction, DropsTombstonesAndTheRecordsTheyBury) {
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 30; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    for (int i = 0; i < 30; i += 2) {
        EXPECT_OK(store->remove("key" + std::to_string(i)));  // Every even key.
    }

    const std::uint64_t before = bytes_on_disk(dir);
    EXPECT_OK(store->compact());
    EXPECT_LT(bytes_on_disk(dir), before);

    // The tombstone *and* the record it buried are both gone from disk, so the
    // merged file holds only the fifteen survivors.
    EXPECT_EQ(store->key_count(), 15u);

    std::string value;
    for (int i = 0; i < 30; ++i) {
        const std::string key = "key" + std::to_string(i);
        if (i % 2 == 0) {
            EXPECT_STATUS(StatusCode::NotFound, store->get(key, &value));
        } else {
            EXPECT_OK(store->get(key, &value));
            EXPECT_EQ(value, "value" + std::to_string(i));
        }
    }
}

TEST(Compaction, MergesEverySealedFileIntoOne) {
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 40; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value"));
    }
    const std::vector<FileId> before = data_file_ids(dir);
    ASSERT_GT(before.size(), 4u);

    EXPECT_OK(store->compact());

    // One merged file plus the empty active file rotation created.
    const std::vector<FileId> after = data_file_ids(dir);
    ASSERT_EQ(after.size(), 2u);
    // The merged file took the *lowest* input id: it is a base layer, so it has
    // to replay before anything that might override it.
    EXPECT_EQ(after.front(), before.front());
    EXPECT_EQ(after.back(), store->active_file_id());
    EXPECT_EQ(store->active_file_size(), 0u);
}

TEST(Compaction, IsANoOpWhenThereIsNothingToReclaim) {
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "value"));
    }
    EXPECT_OK(store->compact());

    const std::uint64_t after_first = bytes_on_disk(dir);
    const std::vector<FileId> ids_after_first = data_file_ids(dir);

    // A second pass has nothing dead to remove, so it must not rewrite the
    // merged file -- that would move every byte in the database to produce an
    // identical one.
    EXPECT_OK(store->compact());
    EXPECT_EQ(bytes_on_disk(dir), after_first);
    EXPECT_EQ(data_file_ids(dir), ids_after_first);
    EXPECT_EQ(store->key_count(), 20u);
}

TEST(Compaction, LeavesKeysLiveInTheActiveFileAlone) {
    // A record whose key's current value lives in the active file is stale by
    // definition and must be dropped, not copied -- copying it would put an old
    // value into a file that replays *after* the one holding the new one.
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "old"));
    }
    // Rewrite half of them; compact() seals the active file first, so these
    // land in the merge as the winning version.
    for (int i = 0; i < 20; i += 2) {
        EXPECT_OK(store->put("key" + std::to_string(i), "new"));
    }

    EXPECT_OK(store->compact());
    store.reset();

    auto reopened = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(reopened, nullptr);
    std::string value;
    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(reopened->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, i % 2 == 0 ? "new" : "old");
    }
}

TEST(Compaction, NeverModifiesAnInputFile) {
    // "Write to a temp file and rename it into place" means no reader is ever
    // looking at a file being edited underneath them.
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall, CompactionFailPoint::AfterWritingTemp));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 30; ++i) {
        EXPECT_OK(store->put("counter", std::to_string(i)));
    }

    std::vector<std::uint64_t> sizes_before;
    const std::vector<FileId> ids = data_file_ids(dir);
    for (const FileId id : ids) {
        sizes_before.push_back(std::filesystem::file_size(data_path(dir, id)));
    }

    EXPECT_OK(store->compact());  // Stops with the output written but uninstalled.

    // Every input is byte-for-byte what it was, and the output is off to the
    // side under a name nothing points at. (compact() seals the active file
    // first, so there is one more data file than there was -- an empty one.)
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(std::filesystem::file_size(data_path(dir, ids[i])), sizes_before[i])
            << "input file " << ids[i] << " was modified";
    }
    // The merged data file and its hint, both still under .tmp names.
    EXPECT_EQ(temp_file_names(dir).size(), 2u);
}

// --- Crashing partway through ----------------------------------------------

// Runs a compaction that stops at `fail_at`, then reopens and checks the
// database is intact. The store is destroyed without any further work, which is
// as close as a unit test can get to the process disappearing.
void expect_survives_crash_at(CompactionFailPoint fail_at) {
    TempDir dir;
    std::vector<std::pair<std::string, std::string>> expected;

    {
        auto store = open_or_fail(dir, tiny_files(kSmall, fail_at));
        ASSERT_NE(store, nullptr);

        for (int i = 0; i < 24; ++i) {
            const std::string key = "key" + std::to_string(i);
            EXPECT_OK(store->put(key, "first"));
        }
        for (int i = 0; i < 24; ++i) {
            const std::string key = "key" + std::to_string(i);
            if (i % 3 == 0) {
                EXPECT_OK(store->remove(key));  // Deleted: must stay deleted.
            } else if (i % 3 == 1) {
                EXPECT_OK(store->put(key, "second"));  // Overwritten.
                expected.emplace_back(key, "second");
            } else {
                expected.emplace_back(key, "first");  // Untouched.
            }
        }

        EXPECT_OK(store->compact());
    }  // The process "dies" here.

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    expect_contents(*store, expected);
    // Whatever state the crash left, reopening tidies the scratch file away.
    EXPECT_TRUE(temp_file_names(dir).empty());

    // And the recovered database is writable and still correct afterwards.
    EXPECT_OK(store->put("written-after-recovery", "ok"));
    store.reset();

    auto again = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(again, nullptr);
    expected.emplace_back("written-after-recovery", "ok");
    expect_contents(*again, expected);
}

TEST(CompactionCrash, BeforeTheOutputIsInstalled) {
    expect_survives_crash_at(CompactionFailPoint::AfterWritingTemp);
}

TEST(CompactionCrash, AfterTheOutputIsInstalledButBeforeAnyInputIsDeleted) {
    // Both the merged file and every input are on disk at once. Correct because
    // the merged file took the highest input id, so it replays last and wins.
    expect_survives_crash_at(CompactionFailPoint::AfterInstallingOutput);
}

TEST(CompactionCrash, PartwayThroughDeletingTheInputs) {
    expect_survives_crash_at(CompactionFailPoint::MidInputDelete);
}

// The layout that puts maximum pressure on both of compaction's ordering rules:
//
//   file 1      padding             <- becomes the merged file's id
//   file 2,3    "gone" is put here  <- an ordinary early file
//   ...         more padding
//   file N      "gone" is tombstoned here, and nothing follows
//
// The merge drops the put and the tombstone alike. Every way of getting the
// ordering wrong ends with the tombstone gone from disk while the put is still
// there -- which is the definition of a key coming back from the dead.
void expect_deleted_key_stays_dead(CompactionFailPoint fail_at) {
    constexpr int kLead = 6;    // Pushes "gone" out of file 1.
    constexpr int kMiddle = 8;  // Pushes the tombstone into a much later file.

    TempDir dir;
    {
        auto store = open_or_fail(dir, tiny_files(kSmall, fail_at));
        ASSERT_NE(store, nullptr);

        for (int i = 0; i < kLead; ++i) {
            EXPECT_OK(store->put("lead" + std::to_string(i), "xxxxxxxx"));
        }
        EXPECT_OK(store->put("gone", "should-not-survive"));
        for (int i = 0; i < kMiddle; ++i) {
            EXPECT_OK(store->put("mid" + std::to_string(i), "xxxxxxxx"));
        }
        // Last write, so compact()'s rotation seals it into the highest input.
        EXPECT_OK(store->remove("gone"));
        ASSERT_GT(store->file_count(), 3u);

        EXPECT_OK(store->compact());
    }

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("gone", &value));
    EXPECT_EQ(store->key_count(), static_cast<std::size_t>(kLead + kMiddle));
}

TEST(CompactionCrash, ADeletedKeyDoesNotComeBackWhenTheMergedFileReplacesItsTombstone) {
    // Why the merged file takes the *lowest* input id.
    //
    // The tombstone is in the highest input file. Install the merged output
    // there -- the obvious choice, since it makes the merge replay last and win
    // -- and installing it destroys that tombstone while the original put still
    // sits in an earlier file. Replay finds the bare put and "gone" lives again.
    //
    // The fail point matters: run to completion and the earlier file is deleted
    // too, so the final state is right and the flaw never shows.
    expect_deleted_key_stays_dead(CompactionFailPoint::AfterInstallingOutput);
}

TEST(CompactionCrash, ADeletedKeyDoesNotComeBackWhenInputsAreHalfDeleted) {
    // Why the inputs are deleted lowest-id-first.
    //
    // Deleting highest-first would remove the file holding the tombstone before
    // the file holding the put it buries, and a crash in that window leaves the
    // put with nothing left to shadow it.
    expect_deleted_key_stays_dead(CompactionFailPoint::MidInputDelete);
}

TEST(CompactionCrash, ALeftoverTempFileIsSweptAwayOnOpen) {
    TempDir dir;
    {
        auto store = open_or_fail(dir, tiny_files(kSmall));
        ASSERT_NE(store, nullptr);
        EXPECT_OK(store->put("alpha", "one"));
    }

    // A merge output that never got renamed. Nothing ever pointed at it, so it
    // is not part of the database -- and replaying it would resurrect whatever
    // it happened to contain.
    const auto stray = dir.path() / temp_name(log_file_name(7));
    testing_support::append_raw(stray, record::encode("ghost", "boo", 1, false));
    ASSERT_TRUE(std::filesystem::exists(stray));

    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, store->get("ghost", &value));
    EXPECT_EQ(store->key_count(), 1u);
    EXPECT_FALSE(std::filesystem::exists(stray));
}

// --- Interaction with the rest of the engine -------------------------------

TEST(Compaction, SurvivesAnEmptyAndAFreshDatabase) {
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    // Nothing sealed, nothing written: no files to merge and nothing to do.
    EXPECT_OK(store->compact());
    EXPECT_EQ(store->file_count(), 1u);
    EXPECT_EQ(store->key_count(), 0u);

    EXPECT_OK(store->put("alpha", "one"));
    EXPECT_OK(store->compact());

    std::string value;
    EXPECT_OK(store->get("alpha", &value));
    EXPECT_EQ(value, "one");
}

TEST(Compaction, WritesContinueNormallyAfterwards) {
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "old"));
    }
    EXPECT_OK(store->compact());

    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(store->put("key" + std::to_string(i), "new"));
    }
    EXPECT_OK(store->remove("key0"));
    EXPECT_OK(store->compact());
    store.reset();

    auto reopened = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->key_count(), 19u);

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, reopened->get("key0", &value));
    for (int i = 1; i < 20; ++i) {
        EXPECT_OK(reopened->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "new");
    }
}

TEST(Compaction, EmptyValuesSurviveTheMerge) {
    // A zero-length value is real data and must stay distinguishable from an
    // absent key -- including through a rewrite of every byte in the database.
    TempDir dir;
    auto store = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(store, nullptr);

    EXPECT_OK(store->put("empty", ""));
    for (int i = 0; i < 20; ++i) {
        EXPECT_OK(store->put("pad" + std::to_string(i), "xxxxxxxx"));
    }
    EXPECT_OK(store->compact());
    store.reset();

    auto reopened = open_or_fail(dir, tiny_files(kSmall));
    ASSERT_NE(reopened, nullptr);
    std::string value = "not-overwritten";
    EXPECT_OK(reopened->get("empty", &value));
    EXPECT_EQ(value, "");
}

}  // namespace
}  // namespace kvstore
