#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/locked_store.hpp"
#include "kvstore/memory_store.hpp"
#include "kvstore/status.hpp"
#include "kvstore_contract.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::TempDir;

// A wrapper that changes behaviour is not a wrapper. Running the same contract
// suite through the lock is what says so.
struct LockedMemoryFactory {
    std::unique_ptr<KVStore> create() {
        inner = std::make_unique<MemoryStore>();
        return std::make_unique<LockedStore>(*inner);
    }

    // Declared first, so it is destroyed *last* -- after the LockedStore that
    // holds a reference to it. Member destruction runs in reverse declaration
    // order, and the fixture's store_ is released before its factory_.
    std::unique_ptr<MemoryStore> inner;
};

INSTANTIATE_TYPED_TEST_SUITE_P(LockedMemoryStore, KVStoreContract, LockedMemoryFactory);

// --- What the lock is actually for ------------------------------------------

TEST(LockedStoreTest, SerialisesConcurrentWriters) {
    // Eight threads into a store that has no idea threads exist. Without the
    // lock this corrupts the inner unordered_map -- and the interesting part is
    // that it usually does so *silently*, which is why the assertion is on the
    // exact final size rather than on not crashing.
    MemoryStore inner;
    LockedStore store{inner};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key = std::to_string(t) + ":" + std::to_string(i);
                EXPECT_OK(store.put(key, "value" + key));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(inner.size(), static_cast<std::size_t>(kThreads * kPerThread));

    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; i += 97) {
            const std::string key = std::to_string(t) + ":" + std::to_string(i);
            std::string value;
            EXPECT_OK(store.get(key, &value));
            EXPECT_EQ(value, "value" + key);
        }
    }
}

TEST(LockedStoreTest, SerialisesReadersAgainstWriters) {
    // Readers are locked too. A get() walking the index while a put() rehashes
    // it is not a stale read, it is a pointer into a freed bucket array.
    MemoryStore inner;
    LockedStore store{inner};

    EXPECT_OK(store.put("stable", "value"));

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < 500; ++i) {
                EXPECT_OK(store.put("churn" + std::to_string(t * 500 + i), "x"));
            }
        });
    }
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&store] {
            for (int i = 0; i < 500; ++i) {
                std::string value;
                EXPECT_OK(store.get("stable", &value));
                EXPECT_EQ(value, "value");
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

TEST(LockedStoreTest, ProtectsTheRealEngine) {
    // The same exercise against Bitcask, which is the store that actually needs
    // it: concurrent appends would race on the index, on the active file's end
    // offset and on the file map at once.
    TempDir dir;
    auto engine = testing_support::open_or_fail(dir, BitcaskOptions{SyncMode::Never});
    ASSERT_NE(engine, nullptr);

    LockedStore store{*engine};

    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                EXPECT_OK(store.put(std::to_string(t) + ":" + std::to_string(i), "v"));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Every key indexed exactly once: no append landed at an offset the index
    // disagrees with, and no index entry was lost to a concurrent insert.
    EXPECT_EQ(engine->key_count(), static_cast<std::size_t>(kThreads * kPerThread));

    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; i += 37) {
            std::string value;
            EXPECT_OK(store.get(std::to_string(t) + ":" + std::to_string(i), &value));
            EXPECT_EQ(value, "v");
        }
    }
}

}  // namespace
}  // namespace kvstore
