#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kvstore/file_names.hpp"

namespace kvstore {

// Where a key's current record lives.
//
// This is the whole reason Bitcask is fast: every live key has exactly one
// entry here, so a read is one hash lookup plus one seek -- never a scan. The
// price is that all keys must fit in memory. Values need not.
//
// Field order is chosen so the struct is exactly 24 bytes with no padding
// (8 + 8 + 4 + 4). There is one of these per live key, so the layout is a real
// memory cost rather than a stylistic choice -- a million keys is 24MB of index
// before counting the keys themselves.
struct ValuePointer {
    std::uint64_t offset = 0;     // Byte offset of the record's first byte.
    std::uint64_t timestamp = 0;  // Copied from the record, so "which is newer"
                                  // is answerable without touching the disk.
    FileId file_id = 0;           // Which data file. Added at rotation: offset
                                  // alone stopped being a unique address the
                                  // moment there was more than one file.
    std::uint32_t size = 0;       // *Total* record size, header included -- a
                                  // reader needs the header and the crc, not
                                  // just the value bytes.
};

class Index {
public:
    // Returns the entry this displaced, if any. The caller wants it because the
    // displaced record is now dead bytes on disk, and knowing which file it was
    // in is what lets the engine report how much compaction would reclaim.
    // Returning it here beats a get() before every put(): one lookup, and the
    // two can't drift apart.
    std::optional<ValuePointer> put(const std::string& key, const ValuePointer& pointer) {
        auto [it, inserted] = map_.try_emplace(key, pointer);
        if (inserted) {
            return std::nullopt;
        }
        const ValuePointer displaced = it->second;
        it->second = pointer;
        return displaced;
    }

    // nullptr when absent. A pointer rather than an optional copy: callers
    // read a field or two and move on.
    [[nodiscard]] const ValuePointer* get(const std::string& key) const {
        const auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }

    // The removed entry, if there was one. Same reason as put().
    std::optional<ValuePointer> erase(const std::string& key) {
        const auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        const ValuePointer removed = it->second;
        map_.erase(it);
        return removed;
    }

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

    // Every live entry, in unspecified order. Diagnostics and tests -- it is
    // what lets a test assert that hint-file recovery rebuilds *exactly* the
    // index a full scan does, rather than merely the same values.
    [[nodiscard]] std::vector<std::pair<std::string, ValuePointer>> snapshot() const {
        std::vector<std::pair<std::string, ValuePointer>> out;
        out.reserve(map_.size());
        for (const auto& [key, pointer] : map_) {
            out.emplace_back(key, pointer);
        }
        return out;
    }

private:
    std::unordered_map<std::string, ValuePointer> map_;
};

}  // namespace kvstore
