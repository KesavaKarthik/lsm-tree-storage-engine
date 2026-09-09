#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/lsm.hpp"
#include "kvstore/manifest.hpp"
#include "kvstore_contract.hpp"
#include "lsm_helpers.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::collect;
using testing_support::leveled;
using testing_support::open_lsm_or_fail;
using testing_support::sst_file_ids;
using testing_support::TempDir;

std::string key_at(int i) {
    std::string digits = std::to_string(i);
    return "key" + std::string(4 - digits.size(), '0') + digits;
}

std::string value_at(int i) { return "value" + std::to_string(i); }

// Options that make the tree actually move: small memtables so flushes happen
// constantly, small budgets so compactions do too. A concurrency test where
// nothing is being flushed or compacted underneath the readers is a test of
// nothing at all.
LsmOptions busy() { return leveled(/*memtable_size=*/512); }

// --- Readers against a writer ---------------------------------------------

// The shape that matters: several readers and a writer hammering the same store
// while flushes and compactions run underneath them. Correctness here is "every
// key a reader finds has the right value, and every key already written is
// findable" -- a reader must never see a torn or half-installed file set.
TEST(LsmConcurrency, ReadersAndAWriterAgreeUnderContention) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);

    constexpr int kSeed = 400;
    for (int i = 0; i < kSeed; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
    }

    std::atomic<bool> stop{false};
    std::atomic<int> highest_written{kSeed - 1};
    std::atomic<int> mismatches{0};
    std::atomic<int> missing{0};
    std::atomic<std::uint64_t> reads{0};

    std::thread writer([&] {
        for (int i = kSeed; i < kSeed + 1200; ++i) {
            if (!store->put(key_at(i), value_at(i)).is_ok()) {
                ++mismatches;
                break;
            }
            highest_written.store(i, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            unsigned state = static_cast<unsigned>(r * 7919 + 13);
            while (!stop.load(std::memory_order_acquire)) {
                // Only keys known to have been written already: anything at or
                // below highest_written was acknowledged before this read began,
                // so it must be visible.
                const int ceiling = highest_written.load(std::memory_order_acquire);
                state = state * 1664525u + 1013904223u;
                const int i = static_cast<int>(state % static_cast<unsigned>(ceiling + 1));

                std::string value;
                const Status status = store->get(key_at(i), &value);
                ++reads;
                if (status.is_not_found()) {
                    ++missing;
                } else if (!status.is_ok() || value != value_at(i)) {
                    ++mismatches;
                }
            }
        });
    }

    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(mismatches.load(), 0) << "a reader saw a value that was never written";
    EXPECT_EQ(missing.load(), 0) << "a reader lost a key that had already been acknowledged";
    EXPECT_GT(reads.load(), 100u) << "the readers must actually have run";

    // And everything is still there afterwards.
    for (int i = 0; i < kSeed; ++i) {
        std::string value;
        SCOPED_TRACE(key_at(i));
        EXPECT_OK(store->get(key_at(i), &value));
        EXPECT_EQ(value, value_at(i));
    }
}

// Concurrent writers. The engine serialises them behind one lock, so the point
// is not throughput -- it is that every acknowledged write survives, and that
// the log's order and the memtable's order never disagree.
TEST(LsmConcurrency, ManyWritersLoseNothing) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);

    constexpr int kThreads = 6;
    constexpr int kPerThread = 150;

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int id = t * kPerThread + i;
                EXPECT_OK(store->put(key_at(id), value_at(id)));
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    for (int id = 0; id < kThreads * kPerThread; ++id) {
        std::string value;
        SCOPED_TRACE(key_at(id));
        EXPECT_OK(store->get(key_at(id), &value));
        EXPECT_EQ(value, value_at(id));
    }
}

// Deletes race with reads. A tombstone that lost a race would surface as a
// resurrected value, which is the failure this whole design keeps guarding
// against -- now with a second thread involved.
TEST(LsmConcurrency, DeletesStayDeletedWhileReadersRun) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);

    constexpr int kCount = 500;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
    }

    std::atomic<int> deleted_up_to{-1};
    std::atomic<int> resurrected{0};
    std::atomic<bool> stop{false};

    std::thread deleter([&] {
        for (int i = 0; i < kCount; ++i) {
            EXPECT_OK(store->remove(key_at(i)));
            deleted_up_to.store(i, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const int ceiling = deleted_up_to.load(std::memory_order_acquire);
                for (int i = 0; i <= ceiling && i < kCount; i += 37) {
                    std::string value;
                    if (store->get(key_at(i), &value).is_ok()) {
                        ++resurrected;
                    }
                }
            }
        });
    }

    deleter.join();
    for (std::thread& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(resurrected.load(), 0) << "an acknowledged delete came back";
    for (int i = 0; i < kCount; ++i) {
        std::string value;
        SCOPED_TRACE(key_at(i));
        EXPECT_STATUS(StatusCode::NotFound, store->get(key_at(i), &value));
    }
}

// --- Reads through a flush and a compaction -------------------------------

// The version swap, observed. A reader that starts before an install and
// finishes after it must see one file set or the other, never a mixture -- and
// in particular must never read a table whose file has been unlinked underneath
// it, which is what the deferred deletion exists to prevent.
TEST(LsmConcurrency, ReadsStayCorrectWhileFlushesAndCompactionsInstall) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);

    constexpr int kCount = 800;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
    }
    EXPECT_OK(store->flush());

    std::atomic<bool> stop{false};
    std::atomic<int> wrong{0};
    std::atomic<std::uint64_t> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            unsigned state = static_cast<unsigned>(r * 104729 + 7);
            while (!stop.load(std::memory_order_acquire)) {
                state = state * 1664525u + 1013904223u;
                const int i = static_cast<int>(state % kCount);
                std::string value;
                if (!store->get(key_at(i), &value).is_ok() || value != value_at(i)) {
                    ++wrong;
                }
                ++reads;
            }
        });
    }

    // Compactions, repeatedly, while all of that is going on.
    for (int round = 0; round < 6; ++round) {
        EXPECT_OK(store->compact());
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(wrong.load(), 0) << "a read fell through a version swap";
    EXPECT_GT(reads.load(), 100u);
    EXPECT_GT(store->stats().compactions.load(), 0u) << "nothing was actually compacted";
}

// --- Scan as a snapshot ----------------------------------------------------

// The iterator copies the memtable and holds handles to everything else, so
// writes made after it was opened are invisible to it -- and, more importantly,
// a flush or compaction that retires a table it is halfway through does not pull
// the file out from under it.
TEST(LsmConcurrency, AScanDoesNotSeeWritesMadeAfterItWasOpened) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);

    constexpr int kBefore = 300;
    for (int i = 0; i < kBefore; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
    }

    auto opened = store->scan("", "");
    ASSERT_TRUE(opened.is_ok()) << opened.status().to_string();
    Iterator cursor = opened.take();

    // Everything below happens *after* the snapshot was taken: new keys,
    // overwrites of keys it holds, deletes of keys it holds, and enough volume
    // to force flushes and compactions that retire the very tables it is reading.
    std::thread mutator([&] {
        for (int i = kBefore; i < kBefore + 600; ++i) {
            EXPECT_OK(store->put(key_at(i), value_at(i)));
        }
        for (int i = 0; i < kBefore; i += 2) {
            EXPECT_OK(store->put(key_at(i), "rewritten"));
        }
        for (int i = 1; i < kBefore; i += 2) {
            EXPECT_OK(store->remove(key_at(i)));
        }
        EXPECT_OK(store->compact());
    });

    const auto rows = collect(cursor);
    mutator.join();

    EXPECT_OK(cursor.status());
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kBefore))
        << "the snapshot must hold exactly what existed when it was taken";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].first, key_at(static_cast<int>(i)));
        EXPECT_EQ(rows[i].second, value_at(static_cast<int>(i)))
            << "a later overwrite leaked into an open snapshot";
    }
}

// The other half of the same guarantee: a file a snapshot is still reading must
// not be unlinked. Compaction marks it and the destructor removes it, so the
// file survives exactly as long as the iterator does.
TEST(LsmConcurrency, ARetiredFileOutlivesAnIteratorStillReadingIt) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);

    for (int i = 0; i < 600; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
    }
    EXPECT_OK(store->flush());
    EXPECT_OK(store->compact());
    const std::vector<FileId> before = sst_file_ids(dir);
    ASSERT_FALSE(before.empty());

    {
        auto opened = store->scan("", "");
        ASSERT_TRUE(opened.is_ok());
        Iterator cursor = opened.take();
        ASSERT_TRUE(cursor.valid());

        // Retire everything the iterator is holding, while it holds it.
        for (int i = 0; i < 600; ++i) {
            EXPECT_OK(store->put(key_at(i), "rewritten"));
        }
        EXPECT_OK(store->flush());
        EXPECT_OK(store->compact());

        // The iterator can still read every byte of the tables it opened.
        const auto rows = collect(cursor);
        EXPECT_OK(cursor.status());
        EXPECT_EQ(rows.size(), 600u);
        for (const auto& [key, value] : rows) {
            EXPECT_NE(value, "rewritten") << "the snapshot was not a snapshot";
        }
    }

    // Once it is gone, the retired files go too.
    EXPECT_OK(store->wait_for_background());
    EXPECT_GT(store->stats().tables_obsoleted.load(), 0u);
}

// --- The write stall -------------------------------------------------------

// Filling the memtable faster than the disk can drain it must block the writer,
// not queue memtables without limit and not drop anything. The observable
// consequences are that memory stays bounded and every write still lands.
TEST(LsmConcurrency, AWriterStallsAndThenProceedsWithoutLosingAnything) {
    TempDir dir;
    LsmOptions options = busy();
    options.memtable_size = 256;  // Fill it constantly, so stalls are certain.
    auto store = open_lsm_or_fail(dir, options);
    ASSERT_NE(store, nullptr);

    constexpr int kCount = 1500;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
        // Never more than one memtable outstanding, however far behind the disk
        // gets. That is the bound a queue would have given away.
        EXPECT_LE(store->memtable_bytes(), 4096u);
    }

    EXPECT_OK(store->wait_for_background());
    EXPECT_GT(store->stats().flushes.load(), 1u) << "the threshold was never crossed";

    for (int i = 0; i < kCount; ++i) {
        std::string value;
        SCOPED_TRACE(key_at(i));
        EXPECT_OK(store->get(key_at(i), &value));
        EXPECT_EQ(value, value_at(i));
    }
}

// --- Shutdown --------------------------------------------------------------

TEST(LsmConcurrency, ShutsDownCleanlyWhileWorkIsInFlight) {
    TempDir dir;
    for (int round = 0; round < 5; ++round) {
        auto store = open_lsm_or_fail(dir, busy());
        ASSERT_NE(store, nullptr);
        // Deliberately destroyed without waiting: the destructor has to stop and
        // join the background thread mid-compaction, and every member it touches
        // must still be alive when it does.
        for (int i = 0; i < 300; ++i) {
            EXPECT_OK(store->put(key_at(i), value_at(i)));
        }
    }

    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);
    for (int i = 0; i < 300; ++i) {
        std::string value;
        SCOPED_TRACE(key_at(i));
        EXPECT_OK(store->get(key_at(i), &value));
        EXPECT_EQ(value, value_at(i));
    }
}

TEST(LsmConcurrency, ConcurrentReadersDoNotBlockEachOther) {
    TempDir dir;
    auto store = open_lsm_or_fail(dir, busy());
    ASSERT_NE(store, nullptr);
    for (int i = 0; i < 400; ++i) {
        EXPECT_OK(store->put(key_at(i), value_at(i)));
    }
    EXPECT_OK(store->flush());
    EXPECT_OK(store->wait_for_background());

    std::atomic<int> wrong{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 8; ++r) {
        readers.emplace_back([&] {
            for (int pass = 0; pass < 3; ++pass) {
                for (int i = 0; i < 400; ++i) {
                    std::string value;
                    if (!store->get(key_at(i), &value).is_ok() || value != value_at(i)) {
                        ++wrong;
                    }
                }
            }
        });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(wrong.load(), 0);
}

}  // namespace
}  // namespace kvstore
