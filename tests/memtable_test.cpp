#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvstore/memtable.hpp"

namespace kvstore {
namespace {

// --- Read your writes ------------------------------------------------------

TEST(MemtableTest, ReadsBackWhatWasWritten) {
    Memtable table;
    table.put("alpha", "one");

    const MemEntry* entry = table.get("alpha");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->value, "one");
    EXPECT_FALSE(entry->tombstone);
}

TEST(MemtableTest, ReturnsNullForAKeyItHasNeverHeardOf) {
    Memtable table;
    table.put("alpha", "one");
    EXPECT_EQ(table.get("beta"), nullptr);
}

TEST(MemtableTest, OverwritingReplacesTheValueWithoutAddingAnEntry) {
    Memtable table;
    table.put("alpha", "first");
    table.put("alpha", "second");

    ASSERT_NE(table.get("alpha"), nullptr);
    EXPECT_EQ(table.get("alpha")->value, "second");
    EXPECT_EQ(table.size(), 1u);
}

TEST(MemtableTest, ValuesAreBinarySafe) {
    Memtable table;
    const std::string key("k\0ey", 4);
    const std::string blob("a\0b\0c", 5);
    table.put(key, blob);

    ASSERT_NE(table.get(key), nullptr);
    EXPECT_EQ(table.get(key)->value, blob);
    EXPECT_EQ(table.get(key)->value.size(), 5u);
}

// --- Deletes ---------------------------------------------------------------

// The distinction the whole design rests on: a delete is a *stored entry*, not
// an absence. An erase would make this memtable silent about the key, and the
// lookup would fall through to an older SSTable and find the old value again.
TEST(MemtableTest, RemoveStoresATombstoneRatherThanErasing) {
    Memtable table;
    table.put("alpha", "one");
    table.remove("alpha");

    const MemEntry* entry = table.get("alpha");
    ASSERT_NE(entry, nullptr) << "a delete must leave something behind to shadow older files";
    EXPECT_TRUE(entry->tombstone);
    EXPECT_EQ(table.size(), 1u);
}

TEST(MemtableTest, RemovingAKeyItHasNeverHeardOfStillStoresATombstone) {
    Memtable table;
    table.remove("ghost");

    const MemEntry* entry = table.get("ghost");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->tombstone);
}

TEST(MemtableTest, WritingOverATombstoneBringsTheKeyBack) {
    Memtable table;
    table.remove("alpha");
    table.put("alpha", "again");

    ASSERT_NE(table.get("alpha"), nullptr);
    EXPECT_FALSE(table.get("alpha")->tombstone);
    EXPECT_EQ(table.get("alpha")->value, "again");
}

// --- Ordering --------------------------------------------------------------

// The reason this is a std::map and not a hash map. Everything downstream --
// the single-pass flush, the sparse index, the merge -- needs this to hold.
TEST(MemtableTest, IteratesInKeyOrderRegardlessOfInsertionOrder) {
    Memtable table;
    table.put("delta", "4");
    table.put("alpha", "1");
    table.put("charlie", "3");
    table.put("bravo", "2");

    std::vector<std::string> keys;
    for (const auto& [key, entry] : table) {
        keys.push_back(key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));
}

TEST(MemtableTest, TombstonesSortAmongTheOtherKeys) {
    Memtable table;
    table.put("a", "1");
    table.remove("b");
    table.put("c", "3");

    std::vector<std::string> keys;
    for (const auto& [key, entry] : table) {
        keys.push_back(key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(MemtableTest, LowerBoundFindsTheFirstKeyAtOrAfterATarget) {
    Memtable table;
    table.put("alpha", "1");
    table.put("charlie", "3");

    EXPECT_EQ(table.lower_bound("bravo")->first, "charlie");
    EXPECT_EQ(table.lower_bound("alpha")->first, "alpha");
    EXPECT_TRUE(table.lower_bound("zulu") == table.end());
}

// --- Size accounting -------------------------------------------------------

TEST(MemtableTest, StartsEmpty) {
    Memtable table;
    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.approximate_bytes(), 0u);
}

TEST(MemtableTest, SizeEstimateGrowsWithTheDataAndCountsTheBytes) {
    Memtable table;
    table.put("alpha", "one");
    const std::uint64_t after_one = table.approximate_bytes();
    EXPECT_GE(after_one, std::string{"alpha"}.size() + std::string{"one"}.size());

    table.put("beta", "two");
    EXPECT_GT(table.approximate_bytes(), after_one);
}

// A workload that rewrites the same keys forever must not look like one that is
// growing without bound -- this estimate is what decides when to flush, and
// double-counting overwrites would flush memtables holding almost nothing.
TEST(MemtableTest, OverwritingDoesNotDoubleCountTheKey) {
    Memtable table;
    table.put("alpha", "one");
    const std::uint64_t after_first = table.approximate_bytes();

    for (int i = 0; i < 100; ++i) {
        table.put("alpha", "one");
    }
    EXPECT_EQ(table.approximate_bytes(), after_first);
}

TEST(MemtableTest, OverwritingWithALongerValueGrowsTheEstimate) {
    Memtable table;
    table.put("alpha", "one");
    const std::uint64_t after_short = table.approximate_bytes();

    table.put("alpha", std::string(1000, 'x'));
    EXPECT_GT(table.approximate_bytes(), after_short + 900);
}

TEST(MemtableTest, ClearResetsBothTheEntriesAndTheEstimate) {
    Memtable table;
    table.put("alpha", "one");
    table.clear();

    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.approximate_bytes(), 0u);
    EXPECT_EQ(table.get("alpha"), nullptr);
}

}  // namespace
}  // namespace kvstore
