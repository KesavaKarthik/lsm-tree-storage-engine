#pragma once

#include <string>
#include <unordered_map>

#include "kvstore/kvstore.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// A KVStore in a hash map. No files, no durability -- it exists to pin down the
// semantics of the interface so Phase 1 only has to get the I/O right.
// Stays useful afterwards as a test double and as the no-disk baseline.
//
// Public header rather than src/: nothing here is worth hiding. The Bitcask
// engine will go the other way (private header + factory), since it has file
// handles and a constructor that can fail.
//
// Not thread-safe.
class MemoryStore final : public KVStore {
public:
    MemoryStore() = default;
    ~MemoryStore() override = default;

    Status put(const std::string& key, const std::string& value) override;
    Status get(const std::string& key, std::string* value) override;
    Status remove(const std::string& key) override;

    // Not part of the contract -- reach for it via the concrete type only.
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

private:
    // Same shape as the eventual Bitcask index; there the value becomes
    // {file id, offset, length}.
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace kvstore
