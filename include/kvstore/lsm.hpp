#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvstore/bloom.hpp"
#include "kvstore/file_names.hpp"
#include "kvstore/kvstore.hpp"
#include "kvstore/log_file.hpp"
#include "kvstore/lsm_stats.hpp"
#include "kvstore/manifest.hpp"
#include "kvstore/memtable.hpp"
#include "kvstore/result.hpp"
#include "kvstore/sstable.hpp"
#include "kvstore/status.hpp"
#include "kvstore/wal.hpp"

namespace kvstore {

class MergingIterator;  // src/lsm_iterator.hpp

// Which tables exist and where, frozen.
//
// A Version is **never mutated after it is built**. A flush or a compaction
// constructs an entirely new one and swaps the pointer; readers that were
// already holding the old one carry on reading it, undisturbed, until they are
// finished. That is the whole concurrency design in one sentence, and it is why
// a reader never has to hold a lock while it touches a disk.
//
// It is also what keeps retired files alive exactly long enough. A table drops
// out of the new Version but stays referenced by the old one, so its file is not
// removed until the last reader lets go -- see SSTable::mark_obsolete().
struct Version {
    std::vector<std::vector<std::shared_ptr<const SSTable>>> levels;
};

// Where a flush should stop, as if the process had died there.
//
// Test-only, and in the production options for the same reason
// CompactionFailPoint is: the states worth testing are the ones flush() actually
// produces, in the order it produces them. A test that stages those directories
// by hand tests the sequence the test author imagined, which is exactly what a
// sequencing bug would also have got wrong.
enum class LsmFailPoint {
    None,
    AfterWritingTable,     // Output written and fsynced under its temp name, not renamed.
    AfterInstallingTable,  // Output renamed into place, but the manifest never committed.
    AfterManifestCommit,   // Manifest committed; the WAL / the old inputs still there.
};

struct LsmOptions {
    // Always by default, for the same reason Bitcask defaults that way: losing
    // acknowledged writes on a power cut should be opted into, not defaulted to.
    SyncMode sync_mode = SyncMode::Always;

    // Flush once the memtable's estimated footprint passes this. Bigger means
    // fewer, larger tables (less compaction work, less read amplification) at
    // the cost of more RAM and a longer WAL to replay after a crash.
    std::uint64_t memtable_size = 4ull << 20;

    std::uint32_t block_size = sstable::kDefaultBlockSize;

    // Bits of bloom filter per key. Ten gives roughly a 1% false-positive rate;
    // zero would disable filters entirely, which BloomBuilder treats as "use the
    // default" rather than "none", so there is deliberately no way to turn them
    // off by accident.
    std::uint32_t bits_per_key = bloom::kDefaultBitsPerKey;

    // Whether the read path consults the filters at all. Filters are still built
    // and loaded when this is false -- only the probe in SSTable::lookup() is
    // skipped, so a benchmark can measure what the filter is worth with both
    // arms reading byte-identical files. Not a tuning knob: leave it true.
    bool bloom_enabled = true;

    // How table data blocks are fetched. Pread by default -- see ReadMode: the
    // faster path cannot report an I/O error, only die of one.
    ReadMode read_mode = ReadMode::Pread;

    // --- Leveled compaction ------------------------------------------------

    // L0 tables overlap each other, so a lookup has to ask every one of them.
    // Four is the point at which merging them into L1 is worth the write.
    std::size_t l0_compaction_trigger = 4;

    // Byte budget for L1; each level below is `level_multiplier` times bigger.
    // A fanout of ten is the standard choice: it makes the number of levels
    // logarithmic in the data size while keeping each merge's write
    // amplification to roughly the fanout.
    std::uint64_t level_base_bytes = 10ull << 20;
    std::uint32_t level_multiplier = 10;

    // Compaction starts a new output table once one passes this, so a single
    // merge produces several bounded files rather than one enormous one.
    std::uint64_t target_table_size = 2ull << 20;

    // Enough for a petabyte at a fanout of ten. A cap at all, because the
    // budget calculation multiplies and must not overflow.
    std::size_t max_levels = 7;

    // Test-only; see LsmFailPoint.
    LsmFailPoint fail_at = LsmFailPoint::None;
};

// A cursor over the whole store in key order: the memtable, the memtable being
// flushed, and every SSTable, merged.
//
// This is the second Bitcask limitation lifted. Bitcask's index is an
// unordered_map over records stored in write order, so "every key from `a` to
// `b`" has no answer short of reading the entire database. Here every source is
// already sorted, so the answer is a k-way merge that touches only the range
// asked for.
//
// **A point-in-time snapshot.** Writes made after scan() returns are not
// visible, and no lock is held while iterating. It is safe to hold one open for
// as long as you like: it owns a copy of the memtable and handles to every table
// it might read, so a flush or a compaction underneath it changes nothing it can
// see, and retires no file it is still inside.
class Iterator {
public:
    ~Iterator();
    Iterator(Iterator&&) noexcept;
    Iterator& operator=(Iterator&&) noexcept;
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    [[nodiscard]] bool valid() const;

    // Valid only while valid() and until the next next(). Precondition: valid().
    [[nodiscard]] std::string_view key() const;
    [[nodiscard]] std::string_view value() const;

    // Advances to the next live key. Tombstones are consumed silently: a
    // deleted key is not a row with no value, it is not a row.
    [[nodiscard]] Status next();

    // Why iteration stopped. Ok when it stopped because the range ran out.
    [[nodiscard]] Status status() const;

private:
    friend class LsmStore;
    explicit Iterator(std::unique_ptr<MergingIterator> impl);

    std::unique_ptr<MergingIterator> impl_;
};

// A log-structured merge-tree, alongside -- not replacing -- Bitcask.
//
//   put    -- append to the WAL (durable), insert into the sorted memtable.
//   get    -- memtable, then the memtable being flushed, then every L0 table
//             newest first, then at most one table per deeper level, stopping
//             at the first table that has an opinion.
//   remove -- the same as put, with a tombstone. Deleting is a write.
//   flush  -- turn the memtable into an immutable sorted SSTable, retire its WAL.
//
// **What this buys over Bitcask, and what it costs.** Bitcask keeps a pointer
// for every live key in RAM, so the keyspace is bounded by memory; here only a
// sparse index -- one key per block -- stays resident, so the keyspace is bounded
// by disk. Bitcask's records are in write order, so range queries are impossible;
// here everything on disk is sorted, so they are a merge. The price is read
// amplification: Bitcask answers a get() with exactly one seek, while this may
// consult several tables before finding the key. Step 2's bloom filters and
// leveled compaction exist to bound that price.
//
// **Thread-safe.** Readers run concurrently with each other and with a writer;
// flushes and compactions run on a background thread and are installed by
// swapping an immutable Version, so a reader never blocks on one and never sees
// a half-installed set of files. Unlike Bitcask it does not need LockedStore --
// which is the entire point of the exercise, since LockedStore made the Phase 3
// server exactly as concurrent as the engine, namely not at all.
class LsmStore final : public KVStore {
public:
    // Opens (creating if needed) the directory: sweeps temp files, opens every
    // SSTable, replays any WAL left behind by a crash, and starts a fresh WAL.
    [[nodiscard]] static Result<std::unique_ptr<LsmStore>> open(
        const std::filesystem::path& directory, const LsmOptions& options = {});

    // Stops and joins the background thread. Out of line and explicit, because
    // the join has to happen *before* any member the thread touches is
    // destroyed -- the same member-ordering hazard the Phase 3 server fixture
    // documents, which on Windows fails every time rather than racing.
    ~LsmStore() override;

    Status put(const std::string& key, const std::string& value) override;
    Status get(const std::string& key, std::string* value) override;
    Status remove(const std::string& key) override;

    // Turns the current memtable into an SSTable, whatever its size, and returns
    // once it is durable. A no-op if the memtable is empty.
    //
    // The work itself happens on the background thread and this waits for it --
    // so that every install, whoever asked for it, happens on one thread. That
    // is what makes "one thread cannot race itself over the same tables" a
    // property of the design rather than a hope about timing.
    [[nodiscard]] Status flush();

    // Forces the WAL out. Meaningful only under SyncMode::Never.
    [[nodiscard]] Status sync();

    // Runs compaction steps until no level is over its budget.
    //
    // Compaction also happens on its own, one bounded step after each flush --
    // which Bitcask's compact() deliberately does not do, and the difference is
    // that a leveled step merges one table against its overlaps rather than
    // every sealed file in the database. A bounded amount of work can be hidden
    // inside a write; an unbounded amount has to be the caller's decision.
    // This is the handle for tests and for a caller that wants it now.
    [[nodiscard]] Status compact();

    // Every live key in [begin, end), in order. An empty `end` means unbounded;
    // an empty `begin` means from the start. (Keys may not be empty, so neither
    // is ambiguous.)
    [[nodiscard]] Result<Iterator> scan(std::string_view begin, std::string_view end) const;

    // Blocks until the background thread is idle. Only useful to a test that
    // wants to observe a settled tree; ordinary callers never need it.
    [[nodiscard]] Status wait_for_background();

    // --- Diagnostics. Not part of the KVStore contract. ---------------------

    // All of these take the lock, so none of them is inline any more. They are
    // diagnostics: a test wants a settled answer, and paying a shared lock for
    // one is cheaper than reasoning about a torn read of a vector.
    [[nodiscard]] const LsmStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t table_count() const noexcept;
    [[nodiscard]] std::size_t level_count() const noexcept;
    [[nodiscard]] std::size_t tables_at(std::size_t level) const noexcept;
    [[nodiscard]] std::uint64_t level_bytes(std::size_t level) const noexcept;
    [[nodiscard]] std::size_t memtable_entries() const noexcept;
    [[nodiscard]] std::uint64_t memtable_bytes() const noexcept;
    [[nodiscard]] std::uint64_t total_disk_size() const;

    // Newest first, which is also the order a get() consults them in: L0
    // descending by id, then each deeper level in key order.
    [[nodiscard]] std::vector<FileId> table_ids() const;
    [[nodiscard]] std::vector<FileId> table_ids_at(std::size_t level) const;

private:
    // The level set. levels_[0] holds L0, newest id first, ranges free to
    // overlap; levels_[i] for i > 0 holds tables sorted by min_key with disjoint
    // ranges, which is what lets a lookup binary-search a level and consider
    // exactly one table.
    //
    // shared_ptr rather than unique_ptr from the start: Step 3 swaps whole level
    // sets under readers, and a reader holding a retired table alive until it is
    // finished with it is the mechanism. Paying for the refcount now means that
    // change adds an atomic swap rather than re-plumbing this container twice.
    using Levels = std::vector<std::vector<std::shared_ptr<const SSTable>>>;
    LsmStore(std::filesystem::path directory, const LsmOptions& options)
        : dir_(std::move(directory)), options_(options) {}

    [[nodiscard]] Status recover();
    [[nodiscard]] Status sweep_temp_files();
    [[nodiscard]] Status sync_dir() const;

    // Opens every table the manifest names, and deletes every .sst it does not.
    [[nodiscard]] Status open_from_manifest(const manifest::LevelSet& set);

    // Deletes every .sst not in `named`. See manifest.hpp: this single rule is
    // the cleanup for a crash *before* a commit and for one *after* it alike.
    [[nodiscard]] Status sweep_orphan_tables(std::vector<FileId> named);

    // A directory with no manifest is one written before manifests existed, not
    // an empty one: every table it holds goes to L0, ordered by id.
    [[nodiscard]] Status adopt_unmanifested_layout();

    // The manifest is a pure projection of the in-memory level set, so the two
    // cannot drift apart -- there is only one place the truth lives.
    [[nodiscard]] manifest::LevelSet level_set_of(const Levels& levels, FileId next_id) const;

    // Makes `levels` durable. Returning Ok is the commit: after this the files
    // it no longer names may be deleted, and before it they may not.
    [[nodiscard]] Status commit_manifest(const Levels& levels, FileId next_id) const;

    [[nodiscard]] Status write_entry(std::string_view key, std::string_view value, bool tombstone);

    // --- Leveled compaction (src/lsm_compaction.cpp) -----------------------

    // One unit of work: some tables from `level`, everything they overlap in
    // `level + 1`, and the merged range those cover.
    struct Compaction {
        std::size_t level = 0;
        std::vector<std::shared_ptr<const SSTable>> inputs;    // From `level`, newest first.
        std::vector<std::shared_ptr<const SSTable>> overlaps;  // From `level + 1`, in key order.
        std::string begin;
        std::string end;
        bool drop_tombstones = false;
    };

    // The most urgent thing to merge, or nothing if there is nothing to do.
    // Const: picking is pure, and doing the work is a separate step.
    //
    // `force` ignores the budgets and instead merges the shallowest level that
    // has anything below it -- which is how compact() drives the tree down to a
    // single level, and therefore how tombstones ever reach a point where they
    // can be dropped.
    [[nodiscard]] std::optional<Compaction> pick_compaction(bool force = false) const;

    // `installed` reports whether the step actually swapped in a new level set.
    // A step that installs nothing has changed nothing, so the next pick would
    // choose the same work again -- which is a loop, not progress.
    // Takes the caller's exclusive lock and releases it for the I/O, exactly as
    // run_flush does. Only ever called from the background thread, which is what
    // makes "one thread cannot race itself over the same tables" true.
    [[nodiscard]] Status compact_once(Compaction work, bool* installed,
                                      std::unique_lock<std::shared_mutex>& lock);

    [[nodiscard]] std::uint64_t level_budget(std::size_t level) const;

    // level_bytes() without taking the lock, for the paths that already hold it.
    [[nodiscard]] std::uint64_t level_bytes_locked(std::size_t level) const;

    // Whether a tombstone covering [begin, end) may be discarded rather than
    // written into the output.
    [[nodiscard]] bool can_drop_tombstones(std::size_t output_level, std::string_view begin,
                                           std::string_view end) const;

    [[nodiscard]] Status open_wal(FileId id);

    // --- Concurrency (src/lsm.cpp) -----------------------------------------

    // The background thread's loop: flush first, then compaction, then sleep.
    // Flush first because it is what unblocks a stalled writer, and a compaction
    // can always wait a moment longer.
    void background_loop();

    // Both take the lock already held and *release it for the I/O*, re-taking it
    // only to install the result. A flush or a compaction is seconds of disk
    // work; holding a lock readers contend on for that long would make the whole
    // exercise pointless.
    // `progressed` reports whether the operation actually changed anything. An
    // operation that changed nothing would be chosen again immediately, so the
    // loop has to stop rather than repeat it -- see halted_by_fail_point_.
    [[nodiscard]] Status run_flush(std::unique_lock<std::shared_mutex>& lock, bool* progressed);
    [[nodiscard]] Status run_compaction(std::unique_lock<std::shared_mutex>& lock, bool force,
                                        bool* progressed);

    // Moves the live memtable aside and starts a fresh one and a fresh WAL.
    // Caller holds write_mu_ and passes an exclusive lock on mu_.
    [[nodiscard]] Status rotate_memtable(std::unique_lock<std::shared_mutex>& lock);

    [[nodiscard]] bool needs_compaction() const;

    // Rebuilds current_ from levels_. Call after every change to levels_, with
    // mu_ held exclusively; it is the moment the change becomes visible.
    void publish_locked();

    // Writes `table` out as a new SSTable, commits a manifest naming it at L0 of
    // `base`, and returns the resulting level set. Does every bit of its I/O
    // with no lock held, which is why it takes the base set by value rather than
    // reading the member.
    [[nodiscard]] Result<Levels> build_table_unlocked(const Memtable& table, FileId id,
                                                      Levels base, FileId next_id);

    std::filesystem::path dir_;
    LsmOptions options_;
    LsmStats stats_;

    // --- Synchronisation ---------------------------------------------------
    //
    // Two locks, and the split between them is the design.
    //
    //   write_mu_  serialises writers against each other. Held for the whole of
    //              a write, *including the WAL fsync* -- which is why only one
    //              write is in flight at a time, the "single writer path".
    //   mu_        guards the state a reader touches. Held exclusively only for
    //              the memtable insert and the version swap, both of which are
    //              nanoseconds of memory traffic.
    //
    // The fsync therefore happens with mu_ **not** held. That is the entire
    // point: the expensive part of a write, a round trip to the device, does not
    // block a single reader.
    mutable std::shared_mutex mu_;

    // mutable, because total_disk_size() is const and has to take it: the WAL's
    // length is only a stable number while no writer is appending to it.
    mutable std::mutex write_mu_;

    // condition_variable_any, because the waiters hold a shared_mutex rather
    // than a plain one. Used for three conversations at once: telling the
    // background thread there is work, telling a stalled writer the flush is
    // done, and telling flush()/compact() their request has been served.
    mutable std::condition_variable_any cv_;

    std::thread background_;
    bool stopping_ = false;
    bool background_busy_ = false;
    bool compact_requested_ = false;

    // Set when a background operation completed having changed nothing, which in
    // practice means a fail point stopped it part-way. The process is pretending
    // to have died, so the thread stops taking work rather than retrying an
    // operation that will always stop in the same place.
    //
    // This is the third appearance of one lesson: a fail point returns, and
    // whatever called it must not carry on. recover() learned it by deleting a
    // WAL it should not have, compact() by spinning ten thousand times, and the
    // background loop by hanging every crash test in the suite.
    bool halted_by_fail_point_ = false;

    // Sticky. Once a background flush or compaction fails there is no safe way
    // to carry on -- the memtable cannot be drained, so writes would either
    // stall forever or grow memory without bound. Recording it and refusing
    // further writes is the honest response, and it is what RocksDB does too.
    Status background_status_;

    // --- Engine state, all guarded by mu_ ----------------------------------

    // The memtable being written to. shared_ptr because a scan hands out a
    // snapshot of it and that snapshot has to stay alive on its own.
    std::shared_ptr<Memtable> mem_;

    // The memtable being flushed: full, frozen, and still answering reads. It is
    // non-null exactly while a flush is outstanding, and a writer that fills the
    // memtable while it is non-null waits rather than starting a second one.
    std::shared_ptr<const Memtable> imm_;

    // The WAL that fed imm_, deleted once imm_ has become a table on disk.
    FileId imm_wal_id_ = 0;

    // The id the frozen memtable will become. Allocated at rotation, *before*
    // the id of the log that replaces it, so that "higher id means newer" stays
    // a single total order over every file the engine writes -- the claim
    // file_names.hpp makes. Allocating it later, when the flush actually runs,
    // would number the table after a log that holds newer writes.
    FileId imm_table_id_ = 0;

    std::optional<Wal> wal_;
    FileId wal_id_ = 0;

    // Two views of the same thing, and the split is deliberate.
    //
    //   levels_   the authoritative set, edited in place, touched only under an
    //             exclusive lock or during single-threaded recovery.
    //   current_  the published snapshot readers take. Rebuilt from levels_ and
    //             swapped whole, never edited.
    //
    // Keeping both costs one vector-of-vectors of refcount bumps per install --
    // which happens once per flush or compaction, not once per read -- and buys
    // a reader that is holding a Version nobody can mutate underneath it.
    Levels levels_;
    std::shared_ptr<const Version> current_;

    // Per level, the max_key of the table compacted out of it last. The next
    // compaction of that level starts after it, so the work rotates around the
    // keyspace instead of hammering whichever table happens to sort first.
    std::vector<std::string> compact_pointer_;

    // One counter for table ids and WAL ids alike -- see file_names.hpp.
    //
    // **Atomic, because it is the one piece of state allocated from outside the
    // lock.** A compaction runs its I/O unlocked and claims an id for each
    // output table as it goes, while a writer rotating the memtable claims one
    // under the lock. Two unsynchronised increments can return the same number,
    // and two files with the same name is not a race that shows up as a slightly
    // wrong answer -- it is one table silently overwriting another.
    //
    // Found by ThreadSanitizer, which is the only instrument that could have.
    std::atomic<FileId> next_id_{kFirstFileId};
};

}  // namespace kvstore
