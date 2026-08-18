#pragma once

#include <string>

#include "kvstore/status.hpp"

namespace kvstore {

// The storage contract. The implementation gets replaced twice (memory ->
// Bitcask -> LSM) and nothing above this line should notice. It's also the seam
// for a fault-injecting wrapper, which is the only sane way to test recovery.
//
// Thread safety: unspecified here; each implementation says for itself.
class KVStore {
public:
    KVStore() = default;

    // Virtual: callers hold these by base pointer, and for the real engine the
    // derived destructor is what flushes and closes the log.
    virtual ~KVStore() = default;

    // An implementation owns a file descriptor and an append offset; copying it
    // would make two owners of one file. Also suppresses the implicit moves.
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    // Insert or overwrite.
    virtual Status put(const std::string& key, const std::string& value) = 0;

    // On Ok, *value is overwritten. NotFound if absent. On any non-Ok status
    // *value is unspecified -- check the Status first.
    // Out-param rather than optional<string> so a read loop can reuse a buffer.
    virtual Status get(const std::string& key, std::string* value) = 0;

    // Idempotent: removing an absent key is Ok. See memory_store.cpp.
    virtual Status remove(const std::string& key) = 0;
};

}  // namespace kvstore
