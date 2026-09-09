#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace kvstore {

// One memtable entry.
//
// A delete is an *entry*, not an absence. This is the difference that makes an
// LSM work at all: the older SSTables below this memtable still hold the value,
// and nothing can go back and edit them, so "deleted" has to be a positive fact
// stored above them that a lookup meets first. Bitcask writes that fact into the
// log as a tombstone record; here it lives in RAM first and reaches disk when the
// memtable is flushed -- as a sorted entry like any other.
struct MemEntry {
    std::string value;
    bool tombstone = false;
};

// The sorted, in-memory front of the write path.
//
// Every write lands here (after the WAL has made it durable) and nowhere else.
// Reads consult it before any file, so it holds the newest version of every key
// it knows about. When it grows past the configured threshold it is flushed, in
// one pass, to an immutable SSTable -- and because it is already sorted, that
// flush is a sequential write with no sorting step.
//
// **Why std::map and not a hash map.** Sorted order is the entire point. A
// memtable that flushed in hash order would produce an unsorted file, and an
// unsorted file has no sparse index, no binary search and no way to merge with
// another file in one pass. The ordering is not a nicety attached to the
// structure; it is the structure's reason for existing.
//
// **Why std::map and not a hand-written skip list.** A red-black tree and a skip
// list have the same asymptotics and comparable constants, so single-threaded
// there is nothing to win. The real argument for a skip list is concurrency: it
// can be built single-writer/multi-reader with no lock on the read side, which
// would let a get() probe the memtable while a put() is inserting. Until the
// engine is concurrent that buys nothing, and Step 3 pays for it instead with a
// shared lock held only for the length of the probe.
//
// std::less<> rather than the default: it makes the map's comparator
// transparent, so a lookup can be done with a std::string_view without
// allocating a std::string to hold a key we already have.
class Memtable {
public:
    using Map = std::map<std::string, MemEntry, std::less<>>;

    void put(std::string_view key, std::string_view value);

    // Inserts a tombstone. Deliberately not an erase: erasing would make this
    // memtable silent about the key, and the lookup would fall through to an
    // older SSTable and resurrect the value.
    void remove(std::string_view key);

    // nullptr means "this memtable has never heard of that key" -- keep looking
    // in older files. A returned entry with tombstone == true is an *answer*:
    // the key is deleted, and the search must stop rather than continue down.
    [[nodiscard]] const MemEntry* get(std::string_view key) const;

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }

    // An estimate, and only ever used to decide when to flush. It counts the key
    // and value bytes plus a fixed guess at per-entry overhead (the map node, its
    // three pointers and colour, and two std::string headers). Being wrong by a
    // factor of a few changes how often a flush happens, not whether anything is
    // correct -- which is why an estimate is the right tool and sizeof-accurate
    // accounting would be wasted precision.
    [[nodiscard]] std::uint64_t approximate_bytes() const noexcept { return bytes_; }

    // Sorted iteration, which is what a flush and a scan both need.
    [[nodiscard]] Map::const_iterator begin() const noexcept { return map_.begin(); }
    [[nodiscard]] Map::const_iterator end() const noexcept { return map_.end(); }
    [[nodiscard]] Map::const_iterator lower_bound(std::string_view key) const {
        return map_.lower_bound(key);
    }

    void clear() noexcept;

private:
    // Roughly what one entry costs beyond its bytes. See approximate_bytes().
    static constexpr std::uint64_t kEntryOverhead = 64;

    void insert(std::string_view key, std::string_view value, bool tombstone);

    Map map_;
    std::uint64_t bytes_ = 0;
};

}  // namespace kvstore
