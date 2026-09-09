#include "kvstore/memtable.hpp"

#include <string>
#include <utility>

namespace kvstore {

namespace {

std::uint64_t entry_bytes(std::string_view key, std::string_view value) {
    return static_cast<std::uint64_t>(key.size()) + value.size() + 64;
}

}  // namespace

void Memtable::put(std::string_view key, std::string_view value) {
    insert(key, value, /*tombstone=*/false);
}

void Memtable::remove(std::string_view key) {
    insert(key, std::string_view{}, /*tombstone=*/true);
}

void Memtable::insert(std::string_view key, std::string_view value, bool tombstone) {
    // Overwriting a key must not double-count it. The size estimate drives the
    // flush threshold, and a workload that rewrites the same keys forever would
    // otherwise appear to grow without bound and flush a memtable holding almost
    // nothing.
    const auto it = map_.find(key);
    if (it != map_.end()) {
        bytes_ -= entry_bytes(it->first, it->second.value);
        it->second.value.assign(value);
        it->second.tombstone = tombstone;
        bytes_ += entry_bytes(key, value);
        return;
    }

    MemEntry entry;
    entry.value.assign(value);
    entry.tombstone = tombstone;
    map_.emplace(std::string{key}, std::move(entry));
    bytes_ += entry_bytes(key, value);
}

const MemEntry* Memtable::get(std::string_view key) const {
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

void Memtable::clear() noexcept {
    map_.clear();
    bytes_ = 0;
}

}  // namespace kvstore
