#include "kvstore/locked_store.hpp"

namespace kvstore {

// lock_guard rather than a manual lock/unlock pair: an early return from the
// inner call -- and every one of these can return early -- must still release
// the mutex. Same argument as LogFile owning its descriptor, one scope down.

Status LockedStore::put(const std::string& key, const std::string& value) {
    const std::lock_guard<std::mutex> guard{mu_};
    return inner_.put(key, value);
}

Status LockedStore::get(const std::string& key, std::string* value) {
    // Reads are locked too, and it is worth being explicit about why: a get()
    // walks the in-memory index while another thread's put() may be inserting
    // into it, and an unordered_map being rehashed under a reader is not a stale
    // answer, it is a pointer into a freed bucket array.
    const std::lock_guard<std::mutex> guard{mu_};
    return inner_.get(key, value);
}

Status LockedStore::remove(const std::string& key) {
    const std::lock_guard<std::mutex> guard{mu_};
    return inner_.remove(key);
}

}  // namespace kvstore
