#include "kvstore/lsm.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

#include "kvstore/record.hpp"
#include "lsm_iterator.hpp"
#include "platform_file.hpp"

namespace kvstore {

namespace {

// The one table in a level below L0 that could hold `key`, or nullptr.
//
// Tables in such a level have disjoint ranges and are sorted by min_key, so the
// first whose max_key reaches the target is the only candidate -- everything
// before it ends too early, everything after begins too late. One binary search
// per level, however many tables the level holds. That is the read-side payoff
// of keeping the levels disjoint, and it is the difference between a lookup
// costing O(levels) and O(tables).
const std::shared_ptr<const SSTable>* find_in_level(
    const std::vector<std::shared_ptr<const SSTable>>& level, std::string_view key) {
    const auto it = std::lower_bound(level.begin(), level.end(), key,
                                     [](const std::shared_ptr<const SSTable>& table,
                                        std::string_view target) {
                                         return std::string_view{table->max_key()} < target;
                                     });
    if (it == level.end() || key < std::string_view{(*it)->min_key()}) {
        return nullptr;
    }
    return &*it;
}

}  // namespace

Result<std::unique_ptr<LsmStore>> LsmStore::open(const std::filesystem::path& directory,
                                                 const LsmOptions& options) {
    if (options.block_size == 0) {
        return Status::invalid_argument("block size must not be zero");
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return Status::io_error("create directory " + directory.string() + ": " + ec.message());
    }

    // `new` rather than make_unique: the constructor is private.
    std::unique_ptr<LsmStore> store{new LsmStore{directory, options}};
    store->mem_ = std::make_shared<Memtable>();

    // Recovery runs single-threaded, before the background thread exists, so
    // none of it needs a lock -- which is why it reads exactly as it did when
    // the engine had no threads at all.
    KVSTORE_RETURN_IF_ERROR(store->recover());
    store->publish_locked();

    LsmStore* raw = store.get();
    store->background_ = std::thread([raw] { raw->background_loop(); });
    return store;
}

// Rebuilds the whole engine state from what is on disk.
//
// The end state is always the same shape regardless of how the last process
// died: every SSTable open and ordered, an empty memtable, and exactly one
// empty WAL. Getting there means answering one question -- what does an
// unflushed memtable's WAL mean when we find it? -- and the answer taken here is
// **flush it immediately**, before deleting it.
//
// The alternative, adopting the WAL and continuing to append to it, is what a
// production engine does and is a little faster to start. It was rejected
// because it makes the invariant conditional: after a crash mid-flush there can
// be *two* WALs, and "the memtable's contents are the union of these logs, one
// of which is still being written to" is a state every later change has to keep
// working. Flushing at open costs one small table exactly when there was
// unflushed work to do, and buys an invariant with no cases in it.
Status LsmStore::recover() {
    // A fail point models the process dying inside flush() or a compaction.
    // Letting one fire inside *recovery* would model something else entirely --
    // a crash that stops half-way and then carries on running, which is not a
    // thing that happens. Concretely it would make write_table() return Ok
    // without having installed anything, after which the code below would
    // cheerfully delete the log that held the only other copy. Found by
    // RepeatedCrashesAtEveryStageStillConverge, which lost three keys.
    struct FailPointPause {
        LsmFailPoint* slot;
        LsmFailPoint saved;
        explicit FailPointPause(LsmFailPoint* s) : slot(s), saved(*s) {
            *s = LsmFailPoint::None;
        }
        ~FailPointPause() { *slot = saved; }
        FailPointPause(const FailPointPause&) = delete;
        FailPointPause& operator=(const FailPointPause&) = delete;
    } const pause{&options_.fail_at};

    KVSTORE_RETURN_IF_ERROR(sweep_temp_files());

    auto stored = manifest::read(dir_);
    if (!stored.is_ok()) {
        return stored.status();
    }
    const bool had_manifest = stored->has_value();
    if (had_manifest) {
        KVSTORE_RETURN_IF_ERROR(open_from_manifest(**stored));
    } else {
        KVSTORE_RETURN_IF_ERROR(adopt_unmanifested_layout());
    }

    // WAL ids are drawn from the same counter as table ids, but a log is opened
    // *after* the manifest that preceded it was committed -- so the persisted
    // next_id can legitimately be behind a log that exists on disk. Take the
    // larger of the two, or the next log would collide with the old one.
    std::vector<FileId> wal_ids;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (const std::optional<FileId> id =
                parse_wal_file_id(entry.path().filename().string())) {
            wal_ids.push_back(*id);
        }
    }
    if (ec) {
        return Status::io_error("read directory " + dir_.string() + ": " + ec.message());
    }
    std::sort(wal_ids.begin(), wal_ids.end());
    for (const FileId id : wal_ids) {
        if (id == std::numeric_limits<FileId>::max()) {
            return Status::io_error("file ids exhausted");
        }
        next_id_.store(std::max(next_id_.load(), static_cast<FileId>(id + 1)));
    }

    // Oldest log first, so a later log's writes overwrite an earlier one's --
    // the same rule as within a single log, for the same reason.
    for (const FileId id : wal_ids) {
        auto wal = Wal::open(dir_ / wal_file_name(id), options_.sync_mode);
        if (!wal.is_ok()) {
            return wal.status();
        }
        Wal opened = wal.take();
        auto replayed = opened.replay_into(mem_.get());
        if (!replayed.is_ok()) {
            return replayed.status();
        }
        LsmStats::bump(stats_.wal_records_replayed, *replayed);
        KVSTORE_RETURN_IF_ERROR(opened.close());
    }

    // Install the replayed writes as a table *before* deleting the logs that
    // hold them. The reverse order has a window in which the only durable copy
    // has been unlinked and the new one does not exist yet.
    if (!mem_->empty()) {
        // Recovery runs before the background thread is started, so it can call
        // the unlocked builder directly -- there is nobody to lock against yet.
        const FileId id = next_id_++;
        auto built = build_table_unlocked(*mem_, id, levels_, next_id_.load());
        if (!built.is_ok()) {
            return built.status();
        }
        levels_ = built.take();
        mem_->clear();
    } else if (!had_manifest) {
        // Nothing to flush, but the directory still owes us a manifest so the
        // next open takes the managed path instead of migrating all over again.
        KVSTORE_RETURN_IF_ERROR(commit_manifest(levels_, next_id_.load()));
    }

    for (const FileId id : wal_ids) {
        const std::filesystem::path path = dir_ / wal_file_name(id);
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        if (remove_ec) {
            return Status::io_error("remove " + path.string() + ": " + remove_ec.message());
        }
    }
    if (!wal_ids.empty()) {
        KVSTORE_RETURN_IF_ERROR(sync_dir());
    }

    return open_wal(next_id_++);
}

Status LsmStore::open_from_manifest(const manifest::LevelSet& set) {
    next_id_.store(set.next_id);
    levels_.clear();
    levels_.resize(set.levels.size());

    std::vector<FileId> named;
    named.reserve(set.table_count());

    for (std::size_t level = 0; level < set.levels.size(); ++level) {
        for (const manifest::TableMeta& meta : set.levels[level]) {
            if (meta.id >= next_id_.load()) {
                return Status::corruption("manifest names a table at or beyond next_id");
            }

            auto opened = SSTable::open(dir_ / sst_file_name(meta.id), meta.id, &stats_,
                                        options_.read_mode, options_.bloom_enabled);
            if (!opened.is_ok()) {
                return opened.status();
            }
            std::shared_ptr<const SSTable> table = opened.take();

            // The manifest is metadata *about* a file, and the file describes
            // itself. Checking that the two agree costs three comparisons at
            // open and catches a manifest that has drifted from what is on
            // disk -- which is checkable at all only because Step 1 made tables
            // self-describing instead of trusting an external index.
            if (table->entry_count() != meta.entry_count || table->min_key() != meta.min_key ||
                table->max_key() != meta.max_key) {
                return Status::corruption("manifest disagrees with " + sst_file_name(meta.id));
            }

            named.push_back(meta.id);
            levels_[level].push_back(std::move(table));
        }
    }

    return sweep_orphan_tables(std::move(named));
}

Status LsmStore::sweep_orphan_tables(std::vector<FileId> named) {
    std::sort(named.begin(), named.end());

    bool removed_any = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        const std::optional<FileId> id = parse_sst_file_id(entry.path().filename().string());
        if (!id.has_value() || std::binary_search(named.begin(), named.end(), *id)) {
            continue;
        }
        // The manifest does not name it, and the manifest is the only thing that
        // makes a table real. So it is either an output from a flush or
        // compaction that never committed, or an input to one that did --
        // unreachable either way, and nothing will ever look at it again.
        std::error_code remove_ec;
        std::filesystem::remove(entry.path(), remove_ec);
        if (remove_ec) {
            return Status::io_error("remove " + entry.path().string() + ": " +
                                    remove_ec.message());
        }
        LsmStats::bump(stats_.tables_obsoleted);
        removed_any = true;
    }
    if (ec) {
        return Status::io_error("read directory " + dir_.string() + ": " + ec.message());
    }
    if (removed_any) {
        KVSTORE_RETURN_IF_ERROR(sync_dir());
    }
    return {};
}

Status LsmStore::adopt_unmanifested_layout() {
    // No manifest means a database written before manifests existed, not an
    // empty one -- so every table present is real and there is nothing to sweep.
    // Running the orphan rule here would delete the entire database.
    std::vector<FileId> ids;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (const std::optional<FileId> id =
                parse_sst_file_id(entry.path().filename().string())) {
            ids.push_back(*id);
        }
    }
    if (ec) {
        return Status::io_error("read directory " + dir_.string() + ": " + ec.message());
    }
    std::sort(ids.begin(), ids.end());

    next_id_.store(kFirstFileId);
    for (const FileId id : ids) {
        if (id == std::numeric_limits<FileId>::max()) {
            return Status::io_error("file ids exhausted");
        }
        next_id_.store(std::max(next_id_.load(), static_cast<FileId>(id + 1)));
    }

    // Every one of them is an L0 table, which is exactly what it was before
    // levels existed: an unordered pile where higher id means newer.
    levels_.clear();
    levels_.resize(1);
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        auto opened = SSTable::open(dir_ / sst_file_name(*it), *it, &stats_,
                                    options_.read_mode, options_.bloom_enabled);
        if (!opened.is_ok()) {
            return opened.status();
        }
        levels_[0].push_back(opened.take());
    }
    return {};
}

// The manifest is a pure projection of the in-memory level set. There is one
// place the truth lives, and this renders it -- so the two cannot drift.
manifest::LevelSet LsmStore::level_set_of(const Levels& levels, FileId next_id) const {
    manifest::LevelSet set;
    set.next_id = next_id;
    set.levels.resize(levels.size());
    for (std::size_t level = 0; level < levels.size(); ++level) {
        set.levels[level].reserve(levels[level].size());
        for (const std::shared_ptr<const SSTable>& table : levels[level]) {
            manifest::TableMeta meta;
            meta.id = table->id();
            meta.file_size = table->file_size();
            meta.entry_count = table->entry_count();
            meta.min_key = table->min_key();
            meta.max_key = table->max_key();
            set.levels[level].push_back(std::move(meta));
        }
    }
    return set;
}

Status LsmStore::commit_manifest(const Levels& levels, FileId next_id) const {
    return manifest::write_atomic(dir_, level_set_of(levels, next_id));
}

Status LsmStore::sweep_temp_files() {
    bool removed_any = false;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        const std::string name = entry.path().filename().string();
        if (!is_temp_name(name)) {
            continue;
        }
        // A .tmp is by definition a file that was being written when the process
        // stopped. Nothing references it -- the rename that would have given it a
        // real name never happened -- so it is unreachable, not merely unused.
        std::error_code remove_ec;
        std::filesystem::remove(entry.path(), remove_ec);
        if (remove_ec) {
            return Status::io_error("remove " + entry.path().string() + ": " +
                                    remove_ec.message());
        }
        removed_any = true;
    }
    if (ec) {
        return Status::io_error("read directory " + dir_.string() + ": " + ec.message());
    }

    if (removed_any) {
        KVSTORE_RETURN_IF_ERROR(sync_dir());
    }
    return {};
}

Status LsmStore::sync_dir() const {
    if (platform::sync_directory(dir_) != 0) {
        return Status::io_error("fsync directory " + dir_.string() + ": " +
                                platform::last_error());
    }
    return {};
}

Status LsmStore::open_wal(FileId id) {
    auto wal = Wal::open(dir_ / wal_file_name(id), options_.sync_mode);
    if (!wal.is_ok()) {
        return wal.status();
    }
    wal_ = wal.take();
    wal_id_ = id;

    // The file's *name* has to be durable too, not just its future contents: a
    // crash could otherwise leave fsynced WAL entries in a file no directory
    // entry points at, which is the same as having lost them.
    return sync_dir();
}

// --- The KVStore contract --------------------------------------------------

Status LsmStore::put(const std::string& key, const std::string& value) {
    return write_entry(key, value, /*tombstone=*/false);
}

Status LsmStore::remove(const std::string& key) {
    return write_entry(key, std::string_view{}, /*tombstone=*/true);
}

Status LsmStore::write_entry(std::string_view key, std::string_view value, bool tombstone) {
    if (key.empty()) {
        return Status::invalid_argument("key must not be empty");
    }
    // The WAL entry is a Bitcask record, so it inherits the record format's size
    // cap. Checked here rather than discovered inside encode(), so the caller
    // gets InvalidArgument for a bad argument rather than an I/O-shaped error.
    const std::uint64_t payload =
        static_cast<std::uint64_t>(key.size()) + (tombstone ? 0 : value.size());
    if (payload > record::kMaxRecordSize - record::kHeaderSize) {
        return Status::invalid_argument("record exceeds the maximum record size");
    }

    // One writer at a time, for the whole of the write. That is the "single
    // writer path": it makes the log's order the memtable's order without any
    // sequencing scheme, and it means wal_ cannot change under us below.
    const std::lock_guard<std::mutex> writer(write_mu_);

    {
        std::unique_lock<std::shared_mutex> lock(mu_);
        KVSTORE_RETURN_IF_ERROR(background_status_);

        if (mem_->approximate_bytes() >= options_.memtable_size) {
            // **The write stall.** The memtable is full and the previous one is
            // still being flushed, so there is nowhere to put this write. Wait.
            //
            // This is the honest response: memory stays bounded by the
            // configured size, no acknowledged write is lost, and the pause is
            // visible. Queueing a second immutable memtable instead would trade
            // a pause for unbounded memory, which is a crash on a slow disk.
            cv_.wait(lock, [this] {
                return stopping_ || !background_status_.is_ok() || halted_by_fail_point_ ||
                       imm_ == nullptr;
            });
            KVSTORE_RETURN_IF_ERROR(background_status_);
            if (imm_ == nullptr && mem_->approximate_bytes() >= options_.memtable_size) {
                KVSTORE_RETURN_IF_ERROR(rotate_memtable(lock));
            }
        }

        if (!wal_.has_value()) {
            return Status::io_error("no write-ahead log is open");
        }
    }

    // **The fsync happens with mu_ released.** It is the expensive part of a
    // write -- a round trip to the device -- and it is the whole reason the two
    // locks are separate: readers are not blocked by it for a moment. Safe
    // because write_mu_ makes this the only writer, and nothing but a writer
    // ever touches wal_.
    KVSTORE_RETURN_IF_ERROR(wal_->append(key, value, tombstone));

    // Durable first, in memory second, and never the other way round. Until the
    // WAL entry has reached the device there is nothing for recovery to replay.
    {
        std::unique_lock<std::shared_mutex> lock(mu_);
        // A delete is a write: it inserts a tombstone rather than erasing,
        // because erasing would only hide the key here and let an older table
        // answer with the value it still holds.
        if (tombstone) {
            mem_->remove(key);
        } else {
            mem_->put(key, value);
        }
        if (mem_->approximate_bytes() >= options_.memtable_size && imm_ == nullptr) {
            cv_.notify_all();  // Nudge the background thread; do not wait for it.
        }
    }
    return {};
}

Status LsmStore::get(const std::string& key, std::string* value) {
    if (value == nullptr) {
        return Status::invalid_argument("value out-parameter must not be null");
    }
    LsmStats::bump(stats_.gets);
    if (key.empty()) {
        return Status::not_found("no such key: " + key);
    }

    std::shared_ptr<const Version> version;
    {
        // **The memtable probe happens under the lock**, and that is deliberate
        // rather than lazy. A memtable is a live object a writer inserts into;
        // holding a shared_ptr to it would keep it alive but would not make
        // reading it while somebody rebalances a red-black tree safe. The probe
        // is a few hundred nanoseconds of memory access, so the lock is held for
        // about as long as it takes to fail to find something.
        //
        // (This is the lock a hand-written skip list would remove: LevelDB's is
        // single-writer/multi-reader lock-free precisely here. NOTES Phase 4 §3
        // promised this trade and this is where it comes due.)
        std::shared_lock<std::shared_mutex> lock(mu_);

        if (const MemEntry* entry = mem_->get(key)) {
            if (entry->tombstone) {
                return Status::not_found("no such key: " + key);
            }
            value->assign(entry->value);
            return {};
        }
        if (imm_ != nullptr) {
            if (const MemEntry* entry = imm_->get(key)) {
                if (entry->tombstone) {
                    return Status::not_found("no such key: " + key);
                }
                value->assign(entry->value);
                return {};
            }
        }
        version = current_;
    }

    // From here on **no lock is held**. Every bloom probe, index search and
    // block read below runs fully concurrently with other readers and with a
    // writer -- and the Version keeps every table it names alive for the
    // duration, however many compactions retire them meanwhile.
    std::optional<Status> answer;
    const auto ask = [&](const SSTable& table) -> Status {
        LsmStats::bump(stats_.tables_probed);
        auto result = table.lookup(key, value);
        if (!result.is_ok()) {
            return result.status();
        }
        if (*result == Lookup::Found) {
            answer = Status::ok();
        } else if (*result == Lookup::Deleted) {
            answer = Status::not_found("no such key: " + key);
        }
        return {};
    };

    const std::vector<std::vector<std::shared_ptr<const SSTable>>>& levels = version->levels;

    // L0 tables overlap each other -- each one is a whole memtable, dumped --
    // so every one of them has to be asked, newest id first.
    if (!levels.empty()) {
        for (const std::shared_ptr<const SSTable>& table : levels[0]) {
            KVSTORE_RETURN_IF_ERROR(ask(*table));
            if (answer.has_value()) {
                return *answer;
            }
        }
    }

    // Below L0 the ranges within a level are disjoint, so one binary search
    // names the only table that could hold the key: at most one table per
    // level, however many the level holds.
    for (std::size_t level = 1; level < levels.size(); ++level) {
        const std::shared_ptr<const SSTable>* candidate = find_in_level(levels[level], key);
        if (candidate == nullptr) {
            continue;
        }
        KVSTORE_RETURN_IF_ERROR(ask(**candidate));
        if (answer.has_value()) {
            return *answer;
        }
    }

    return Status::not_found("no such key: " + key);
}

Status LsmStore::sync() {
    if (!wal_.has_value()) {
        return {};
    }
    return wal_->sync();
}

// --- Concurrency -----------------------------------------------------------

void LsmStore::publish_locked() {
    auto version = std::make_shared<Version>();
    version->levels = levels_;
    current_ = std::move(version);
}

bool LsmStore::needs_compaction() const { return pick_compaction().has_value(); }

LsmStore::~LsmStore() {
    {
        std::unique_lock<std::shared_mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (background_.joinable()) {
        background_.join();
    }
    // Everything the thread touched is still alive at this point, which is the
    // whole reason this destructor is written out rather than defaulted:
    // members are destroyed in reverse declaration order *after* the body runs,
    // so joining here is what makes that order irrelevant.
}

// Flush first, compaction second, then sleep. A stalled writer is waiting on the
// flush and nothing is waiting on the compaction, so the ordering is not a
// preference -- it is the difference between a pause and a stall.
void LsmStore::background_loop() {
    std::unique_lock<std::shared_mutex> lock(mu_);

    const auto idle = [this] {
        return stopping_ || !background_status_.is_ok() || halted_by_fail_point_;
    };

    while (!stopping_) {
        if (!idle() && imm_ != nullptr) {
            background_busy_ = true;
            bool progressed = false;
            const Status status = run_flush(lock, &progressed);
            if (!status.is_ok()) {
                background_status_ = status;
            } else if (!progressed) {
                halted_by_fail_point_ = true;
            }
            background_busy_ = false;
            cv_.notify_all();
            continue;
        }

        if (!idle() && (compact_requested_ || needs_compaction())) {
            const bool force = compact_requested_;
            background_busy_ = true;
            bool progressed = false;
            const Status status = run_compaction(lock, force, &progressed);
            if (!status.is_ok()) {
                background_status_ = status;
            } else if (!progressed) {
                halted_by_fail_point_ = true;
            }
            compact_requested_ = false;
            background_busy_ = false;
            cv_.notify_all();
            continue;
        }

        cv_.wait(lock);
    }
}

// Called with `lock` held exclusively. Releases it for the I/O and takes it back
// to install -- which is the whole point, since the I/O is seconds of disk work
// and the install is a pointer swap.
Status LsmStore::run_flush(std::unique_lock<std::shared_mutex>& lock, bool* progressed) {
    *progressed = false;
    std::shared_ptr<const Memtable> table = imm_;
    const FileId retiring_wal = imm_wal_id_;
    const FileId id = imm_table_id_;
    Levels base = levels_;
    const FileId next_id_snapshot = next_id_.load();

    lock.unlock();
    auto built = build_table_unlocked(*table, id, std::move(base), next_id_snapshot);
    lock.lock();

    if (!built.is_ok()) {
        return built.status();
    }

    // "The process died here." Both fail points stop before the manifest that
    // would have made the table real, so recovery sweeps it as an orphan.
    if (options_.fail_at == LsmFailPoint::AfterWritingTable ||
        options_.fail_at == LsmFailPoint::AfterInstallingTable) {
        return {};
    }

    levels_ = built.take();
    publish_locked();
    LsmStats::bump(stats_.flushes);

    if (options_.fail_at == LsmFailPoint::AfterManifestCommit) {
        return {};  // Committed, but the log that fed it is never retired.
    }

    // The memtable is on disk, so the log that mirrored it has no job left.
    // Clearing imm_ is what releases a stalled writer, so it happens before the
    // unlink rather than after it.
    imm_.reset();
    *progressed = true;

    lock.unlock();
    std::error_code ec;
    std::filesystem::remove(dir_ / wal_file_name(retiring_wal), ec);
    const Status synced = sync_dir();
    lock.lock();

    if (ec) {
        return Status::io_error("remove " + wal_file_name(retiring_wal) + ": " + ec.message());
    }
    return synced;
}

Status LsmStore::run_compaction(std::unique_lock<std::shared_mutex>& lock, bool force,
                                bool* progressed) {
    *progressed = false;
    for (int step = 0; step < 10000; ++step) {
        if (stopping_) {
            return {};
        }

        std::optional<Compaction> work = pick_compaction();
        if (!work.has_value() && force) {
            work = pick_compaction(/*force=*/true);
        }
        if (!work.has_value()) {
            return {};
        }

        bool installed = false;
        KVSTORE_RETURN_IF_ERROR(compact_once(std::move(*work), &installed, lock));

        // A step that installed nothing changed nothing, so the next pick would
        // choose the same work again. Under a fail point that is expected -- the
        // process is meant to have died -- and looping past it would spin while
        // writing temp files nobody will read.
        if (!installed) {
            return {};
        }
        *progressed = true;

        // Automatic compaction is one bounded step per flush. Only an explicit
        // compact() keeps going until the tree is a single level.
        if (!force) {
            return {};
        }
    }
    return Status::io_error("compaction failed to converge");
}

Status LsmStore::rotate_memtable(std::unique_lock<std::shared_mutex>& lock) {
    // Freeze the live memtable and start a fresh one. From here the read path
    // consults both, and the background thread has work to do.
    imm_ = mem_;
    imm_wal_id_ = wal_id_;

    // The table's id is claimed here, before the replacement log's, so that a
    // higher id still means newer across both kinds of file.
    imm_table_id_ = next_id_++;

    mem_ = std::make_shared<Memtable>();

    const FileId next = next_id_++;
    const std::filesystem::path next_path = dir_ / wal_file_name(next);

    // Two logs exist on disk from here until the flush retires the old one.
    // recover() has always replayed every log it finds, ascending, so this state
    // needed no new recovery code -- it was built for exactly this moment.
    //
    // Safe to do unlocked because write_mu_ makes this the only writer, and
    // nothing else ever touches wal_.
    lock.unlock();
    Status closed = wal_->close();
    auto opened = Wal::open(next_path, options_.sync_mode);
    const Status synced = sync_dir();
    lock.lock();

    KVSTORE_RETURN_IF_ERROR(closed);
    if (!opened.is_ok()) {
        return opened.status();
    }
    KVSTORE_RETURN_IF_ERROR(synced);

    wal_ = opened.take();
    wal_id_ = next;

    cv_.notify_all();  // There is a memtable to flush.
    return {};
}

Status LsmStore::wait_for_background() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    cv_.wait(lock, [this] {
        return stopping_ || !background_status_.is_ok() || halted_by_fail_point_ ||
               (!background_busy_ && imm_ == nullptr && !compact_requested_);
    });
    return background_status_;
}

Status LsmStore::flush() {
    // The rotation is a write, so it takes the writer lock -- otherwise it could
    // move a memtable aside from under a put() that is mid-append to the log.
    {
        const std::lock_guard<std::mutex> writer(write_mu_);
        std::unique_lock<std::shared_mutex> lock(mu_);
        KVSTORE_RETURN_IF_ERROR(background_status_);

        if (mem_->empty() && imm_ == nullptr) {
            return {};
        }
        if (!mem_->empty()) {
            // Wait for any flush already in flight, then freeze this memtable
            // too. Only one may be outstanding at a time.
            cv_.wait(lock, [this] {
                return stopping_ || !background_status_.is_ok() || halted_by_fail_point_ ||
                       imm_ == nullptr;
            });
            KVSTORE_RETURN_IF_ERROR(background_status_);
            if (!stopping_ && imm_ == nullptr && !mem_->empty()) {
                KVSTORE_RETURN_IF_ERROR(rotate_memtable(lock));
            }
        }
    }
    cv_.notify_all();

    // The work happens on the background thread and this waits for it, so that
    // every install -- whoever asked for it -- happens on one thread.
    return wait_for_background();
}

Status LsmStore::compact() {
    {
        std::unique_lock<std::shared_mutex> lock(mu_);
        KVSTORE_RETURN_IF_ERROR(background_status_);
        compact_requested_ = true;
    }
    cv_.notify_all();
    return wait_for_background();
}

// Every line of this runs with no lock held. That is why it takes the base
// level set by value and returns a new one, rather than reading and writing the
// member: the caller snapshots under the lock, releases it for the seconds of
// disk work below, and installs the result under the lock again.
Result<LsmStore::Levels> LsmStore::build_table_unlocked(const Memtable& table, FileId id,
                                                        Levels base, FileId next_id) {
    const std::string name = sst_file_name(id);
    const std::filesystem::path final_path = dir_ / name;
    const std::filesystem::path temp_path = dir_ / temp_name(name);

    auto created = SSTableBuilder::create(temp_path, options_.block_size, options_.bits_per_key);
    if (!created.is_ok()) {
        return created.status();
    }
    SSTableBuilder builder = created.take();

    // std::map iterates in key order, so this is already the sorted pass the
    // file format wants -- no sorting step, which is what paying for an ordered
    // memtable on the write path bought.
    for (const auto& [key, entry] : table) {
        KVSTORE_RETURN_IF_ERROR(builder.add(key, entry.value, entry.tombstone));
    }
    KVSTORE_RETURN_IF_ERROR(builder.finish());
    LsmStats::bump(stats_.bytes_written, builder.file_size());

    if (options_.fail_at == LsmFailPoint::AfterWritingTable) {
        return base;  // Written, never installed. Recovery sweeps the temp.
    }

    if (platform::rename_file(temp_path, final_path) != 0) {
        return Status::io_error("rename " + temp_path.string() + " to " + final_path.string() +
                                ": " + platform::last_error());
    }
    // The rename edited a directory. fsyncing the file made its bytes durable
    // and said nothing about the name that reaches them.
    KVSTORE_RETURN_IF_ERROR(sync_dir());

    auto opened =
        SSTable::open(final_path, id, &stats_, options_.read_mode, options_.bloom_enabled);
    if (!opened.is_ok()) {
        return opened.status();
    }

    // The file now exists under its real name, and it is still not part of the
    // database: nothing references it, so recovery would sweep it as an orphan.
    if (options_.fail_at == LsmFailPoint::AfterInstallingTable) {
        return base;
    }

    if (base.empty()) {
        base.resize(1);
    }
    base[0].insert(base[0].begin(), opened.take());

    // The commit. After this the table is real; before it, it is an orphan.
    KVSTORE_RETURN_IF_ERROR(commit_manifest(base, next_id));
    return base;
}

// --- Scan ------------------------------------------------------------------

Result<Iterator> LsmStore::scan(std::string_view begin, std::string_view end) const {
    std::shared_ptr<const Memtable> snapshot;
    std::shared_ptr<const Memtable> frozen;
    std::shared_ptr<const Version> version;

    {
        std::shared_lock<std::shared_mutex> lock(mu_);

        // **The live memtable is copied.** Everything else here is immutable and
        // can simply be shared, but the memtable is the one thing a writer keeps
        // changing, and an iterator that may be held for minutes cannot keep a
        // lock on it or read it unsynchronised.
        //
        // The copy is bounded by memtable_size -- a few MB at worst -- and buys a
        // real point-in-time snapshot with no lock held while iterating. The
        // alternatives are worse: holding the shared lock for the iterator's
        // whole life turns a caller's scan length into a writer-blocking outage,
        // and a skip list with sequence numbers is how a production engine
        // avoids the copy entirely. That is the second place in this project a
        // skip list would have paid for itself; still not built, because both
        // costs are bounded and neither is measured to hurt.
        snapshot = std::make_shared<const Memtable>(*mem_);
        frozen = imm_;
        version = current_;
    }

    // Newest first, which is what MergingIterator's tie-break depends on.
    std::vector<std::unique_ptr<EntrySource>> sources;
    sources.push_back(std::make_unique<MemtableSource>(std::move(snapshot), begin, end));
    if (frozen != nullptr) {
        sources.push_back(std::make_unique<MemtableSource>(std::move(frozen), begin, end));
    }

    // L0 newest-first, then each deeper level. Within a level below L0 the
    // ranges are disjoint, so the order among those sources cannot matter -- no
    // two of them will ever be positioned on the same key.
    for (const std::vector<std::shared_ptr<const SSTable>>& level : version->levels) {
        for (const std::shared_ptr<const SSTable>& table : level) {
            auto source = TableSource::create(table, begin, end);
            if (!source.is_ok()) {
                return source.status();
            }
            sources.push_back(source.take());
        }
    }

    auto merging = std::make_unique<MergingIterator>(std::move(sources));
    KVSTORE_RETURN_IF_ERROR(merging->seek_to_first());
    return Iterator{std::move(merging)};
}

// --- Diagnostics -----------------------------------------------------------

std::size_t LsmStore::table_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::size_t count = 0;
    for (const auto& level : levels_) {
        count += level.size();
    }
    return count;
}

std::size_t LsmStore::level_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return levels_.size();
}

std::size_t LsmStore::tables_at(std::size_t level) const noexcept {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return level < levels_.size() ? levels_[level].size() : 0;
}

std::uint64_t LsmStore::level_bytes(std::size_t level) const noexcept {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return level_bytes_locked(level);
}

std::uint64_t LsmStore::level_bytes_locked(std::size_t level) const {
    if (level >= levels_.size()) {
        return 0;
    }
    std::uint64_t bytes = 0;
    for (const std::shared_ptr<const SSTable>& table : levels_[level]) {
        bytes += table->file_size();
    }
    return bytes;
}

std::size_t LsmStore::memtable_entries() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return mem_->size();
}

std::uint64_t LsmStore::memtable_bytes() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return mem_->approximate_bytes();
}

std::uint64_t LsmStore::total_disk_size() const {
    // write_mu_ before mu_, the same order every other path takes, because the
    // WAL's size is only stable while no writer is appending to it.
    const std::lock_guard<std::mutex> writer(write_mu_);
    std::shared_lock<std::shared_mutex> lock(mu_);

    std::uint64_t total = 0;
    for (std::size_t level = 0; level < levels_.size(); ++level) {
        total += level_bytes_locked(level);
    }
    if (wal_.has_value()) {
        total += wal_->size();
    }
    return total;
}

std::vector<FileId> LsmStore::table_ids() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<FileId> ids;
    for (const auto& level : levels_) {
        for (const std::shared_ptr<const SSTable>& table : level) {
            ids.push_back(table->id());
        }
    }
    return ids;
}

std::vector<FileId> LsmStore::table_ids_at(std::size_t level) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<FileId> ids;
    if (level >= levels_.size()) {
        return ids;
    }
    for (const std::shared_ptr<const SSTable>& table : levels_[level]) {
        ids.push_back(table->id());
    }
    return ids;
}

}  // namespace kvstore
