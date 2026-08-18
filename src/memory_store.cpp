#include "kvstore/memory_store.hpp"

#include <cassert>

namespace kvstore {

Status MemoryStore::put(const std::string& key, const std::string& value) {
    // On disk the key length is a header field, so a zero-length key would be
    // indistinguishable from a malformed record during recovery. Reject it here
    // too, so both implementations agree on what a valid key is.
    if (key.empty()) {
        return Status::invalid_argument("key must not be empty");
    }

    // operator[], not insert(): insert() silently keeps the old value.
    map_[key] = value;
    return Status::ok();
}

Status MemoryStore::get(const std::string& key, std::string* value) {
    // Caller bug, not a runtime condition: loud in debug, degraded in release.
    assert(value != nullptr && "get() requires a non-null out-parameter");
    if (value == nullptr) {
        return Status::invalid_argument("value out-parameter must not be null");
    }

    const auto it = map_.find(key);
    if (it == map_.end()) {
        // Key in the message: the status may be read far from here.
        return Status::not_found("no such key: " + key);
    }

    // Only written on success -- see the contract in kvstore.hpp.
    *value = it->second;
    return Status::ok();
}

Status MemoryStore::remove(const std::string& key) {
    map_.erase(key);

    // Deliberate: removing an absent key is Ok, not NotFound.
    //  - Retries after a crash must not report a different outcome.
    //  - The log-based engine can't answer cheaply anyway: a delete is a
    //    tombstone append, and mid-replay we don't know what preceded it.
    //  - Matches HTTP DELETE, RocksDB Delete(), S3 DeleteObject.
    // Callers who need "did it exist?" can get() first.
    return Status::ok();
}

}  // namespace kvstore
