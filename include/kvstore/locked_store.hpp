#pragma once

#include <mutex>
#include <string>

#include "kvstore/kvstore.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// Serialises every call into another KVStore behind one mutex.
//
// **Why this exists.** The server puts a thread on every connection, and Bitcask
// says in its own header that it is single-threaded and not safe to share. Two
// threads inside put() at once would race on the index, on the active file's
// end offset, and on the file map -- and the failure would not be a crash but a
// record written at an offset the index does not have, discovered days later by
// a get() returning somebody else's value.
//
// **Why the lock is this coarse.** One mutex over all three operations means the
// server is exactly as concurrent as the engine is, which is to say not at all:
// every get blocks every other get, and a database that fits in page cache
// serves requests one at a time regardless of how many cores are idle. That is
// a real cost and it is accepted on purpose. The alternative worth having is a
// shared_mutex with a single writer and parallel readers, and that is a change
// to the *engine* -- it needs the index to be safe to read while a writer
// appends, which it is not. Faking it here with a shared_mutex would be a lock
// that looks concurrent and protects nothing.
//
// **Why a separate class rather than a mutex inside Server.** Three reasons, and
// the third is the one that matters:
//
//   - Server stays a protocol implementation. It holds a KVStore& and calls
//     three virtuals; it does not know that locking is a thing that happens.
//   - Being a KVStore, this is testable on its own -- it runs through the same
//     KVStoreContract suite as every other implementation, so "the wrapper still
//     behaves like a store" is checked rather than assumed.
//   - It is one file to delete. When the engine grows real reader/writer
//     concurrency, this class disappears and nothing else in the server moves.
//
// **What it does not cover.** Only the three KVStore virtuals. Bitcask::sync()
// and Bitcask::compact() are not on the interface, so a caller reaching for them
// through the concrete pointer is outside this lock entirely and must have no
// connection threads running. See apps/server_main.cpp, which syncs only after
// the server has joined every thread.
//
// Holds a reference, and so does not own: whoever created the inner store must
// outlive this. That is the same arrangement Server has with the store handed
// to it, and both are satisfied by main() owning the engine.
class LockedStore final : public KVStore {
public:
    explicit LockedStore(KVStore& inner) noexcept : inner_(inner) {}

    ~LockedStore() override = default;

    Status put(const std::string& key, const std::string& value) override;
    Status get(const std::string& key, std::string* value) override;
    Status remove(const std::string& key) override;

private:
    KVStore& inner_;
    std::mutex mu_;
};

}  // namespace kvstore
