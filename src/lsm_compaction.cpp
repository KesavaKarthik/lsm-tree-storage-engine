#include <algorithm>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <mutex>
#include <shared_mutex>

#include "kvstore/lsm.hpp"
#include "lsm_iterator.hpp"
#include "platform_file.hpp"

// Leveled compaction.
//
// **Why leveled rather than size-tiered**, since both are respectable and the
// choice shapes everything here.
//
// Size-tiered groups tables of similar size and merges a whole group at once.
// Writes are cheap -- a byte is rewritten roughly once per level it descends --
// but a level holds several overlapping tables, so a lookup may have to consult
// all of them, and just before a big merge the disk holds several full copies of
// the same data. Space amplification of 2x or worse is normal.
//
// Leveled keeps every level below L0 as a set of tables with **disjoint** key
// ranges. That costs more writing: merging one table into the level below
// rewrites it against roughly `fanout` times its own size, so a byte is rewritten
// about ten times per level. What it buys is that a lookup consults **at most one
// table per level**, and that the tree holds barely more than one copy of the
// data -- space amplification near 1.1x.
//
// Chosen because this engine sits behind a request/response protocol where reads
// dominate, and because bounded space is precisely the property Bitcask's
// compaction was built to deliver. An LSM that regressed it would be a step
// backwards dressed up as progress.
namespace kvstore {

namespace {

// Do two inclusive key ranges touch?
bool ranges_overlap(std::string_view a_min, std::string_view a_max, std::string_view b_min,
                    std::string_view b_max) {
    return !(a_max < b_min || b_max < a_min);
}

}  // namespace

std::uint64_t LsmStore::level_budget(std::size_t level) const {
    // L0 is measured in files rather than bytes -- see pick_compaction -- so the
    // byte budget starts at L1 and multiplies from there.
    std::uint64_t budget = options_.level_base_bytes;
    for (std::size_t i = 1; i < level; ++i) {
        budget *= options_.level_multiplier;
    }
    return budget;
}

bool LsmStore::can_drop_tombstones(std::size_t output_level, std::string_view begin,
                                   std::string_view end) const {
    // A tombstone may only be discarded once nothing below can still be holding
    // the value it buries. If any deeper level has a table whose range covers
    // part of this one, that table may contain the key -- and dropping the
    // tombstone would let its value surface again, undoing a delete that may
    // have happened years ago.
    //
    // Computed once for the whole compaction rather than per key: it is a
    // conservative answer (a deeper table that overlaps the *range* may not
    // actually hold any of these keys), but it is cheap, and being conservative
    // here costs disk space while being wrong costs data.
    for (std::size_t level = output_level + 1; level < levels_.size(); ++level) {
        for (const std::shared_ptr<const SSTable>& table : levels_[level]) {
            if (ranges_overlap(begin, end, table->min_key(), table->max_key())) {
                return false;
            }
        }
    }
    return true;
}

std::optional<LsmStore::Compaction> LsmStore::pick_compaction(bool force) const {
    Compaction work;

    if (force) {
        // The shallowest level that still has something beneath it. Merging it
        // down repeatedly collapses the tree into a single level, which is the
        // only state in which a tombstone at the bottom is safe to discard.
        std::size_t chosen = levels_.size();
        for (std::size_t level = 0; level < levels_.size(); ++level) {
            if (levels_[level].empty()) {
                continue;
            }
            const bool anything_below = std::any_of(
                levels_.begin() + static_cast<std::ptrdiff_t>(level) + 1, levels_.end(),
                [](const std::vector<std::shared_ptr<const SSTable>>& deeper) {
                    return !deeper.empty();
                });
            if (anything_below) {
                chosen = level;
                break;
            }
        }
        if (chosen == levels_.size()) {
            return std::nullopt;  // At most one level holds anything: done.
        }
        work.level = chosen;
        work.inputs = levels_[chosen];  // All of it -- this call is explicitly unbounded.
    } else if (!levels_.empty() && levels_[0].size() >= options_.l0_compaction_trigger) {
        // L0 is counted in files, not bytes, and that is the whole reason it is
        // special. Its tables all overlap each other -- each one is a memtable
        // dumped whole -- so a lookup has to ask every one of them, and the
        // trigger is the point at which asking that many is worse than the write
        // it costs to merge them.
        work.level = 0;
        work.inputs = levels_[0];  // All of them: they overlap, so a subset is no cheaper.
    } else {
        // Otherwise, the shallowest level that is over its byte budget. Shallowest
        // first because that is where new data arrives, and letting it back up
        // pushes the overflow onto the read path.
        std::size_t chosen = 0;
        for (std::size_t level = 1; level < levels_.size() && level + 1 < options_.max_levels;
             ++level) {
            if (!levels_[level].empty() && level_bytes_locked(level) > level_budget(level)) {
                chosen = level;
                break;
            }
        }
        if (chosen == 0) {
            return std::nullopt;  // Everything is within budget.
        }

        // One table, starting after wherever the last compaction of this level
        // left off, so the work rotates around the keyspace rather than
        // repeatedly rewriting whichever table happens to sort first.
        const std::vector<std::shared_ptr<const SSTable>>& level = levels_[chosen];
        std::shared_ptr<const SSTable> picked = level.front();
        if (chosen < compact_pointer_.size() && !compact_pointer_[chosen].empty()) {
            for (const std::shared_ptr<const SSTable>& table : level) {
                if (table->min_key() > compact_pointer_[chosen]) {
                    picked = table;
                    break;
                }
            }
        }
        work.level = chosen;
        work.inputs.push_back(std::move(picked));
    }

    // The range the inputs cover, which is what decides who they collide with.
    work.begin = work.inputs.front()->min_key();
    work.end = work.inputs.front()->max_key();
    for (const std::shared_ptr<const SSTable>& table : work.inputs) {
        work.begin = std::min(work.begin, table->min_key());
        work.end = std::max(work.end, table->max_key());
    }

    // Everything in the level below that the merged range touches. These are
    // inputs too: the output has to replace them, or the level would stop being
    // disjoint -- which is the invariant the whole read path depends on.
    const std::size_t output_level = work.level + 1;
    if (output_level < levels_.size()) {
        for (const std::shared_ptr<const SSTable>& table : levels_[output_level]) {
            if (ranges_overlap(work.begin, work.end, table->min_key(), table->max_key())) {
                work.overlaps.push_back(table);
            }
        }
        // Absorbing the overlaps widens the range, which can bring further
        // tables into contact with it. Widen once and re-collect, rather than
        // iterating to a fixed point -- an unbounded expansion is exactly the
        // all-or-nothing merge leveled compaction exists to avoid.
        for (const std::shared_ptr<const SSTable>& table : work.overlaps) {
            work.begin = std::min(work.begin, table->min_key());
            work.end = std::max(work.end, table->max_key());
        }
    }

    work.drop_tombstones = can_drop_tombstones(output_level, work.begin, work.end);
    return work;
}

// The install sequence, which is the flush sequence with the same commit point:
//
//   1. Write every output under a .tmp name, fsync, close.
//   2. Rename them into place, fsync the directory.
//   3. Commit a manifest naming the outputs and not the inputs. **The commit.**
//   4. Close and delete the inputs, fsync the directory.
//
// A crash before (3) leaves outputs the manifest does not name; a crash after it
// leaves inputs the manifest no longer names. Both are orphans, and recovery
// deletes them under one rule without needing to know which happened. Nothing is
// ever mutated in place, and no reader is ever pointed at a file that is being
// written -- the outputs do not exist under their real names until (2), and the
// inputs stay perfectly readable until (4).
Status LsmStore::compact_once(Compaction work, bool* installed,
                              std::unique_lock<std::shared_mutex>& lock) {
    if (installed != nullptr) {
        *installed = false;
    }

    // Everything from here to the install is I/O -- reading every input, writing
    // every output, fsyncing each one. It runs with the lock released, so
    // readers carry on through the *old* Version the whole time. Only this
    // thread ever installs, so nothing can change levels_ while we are away.
    lock.unlock();

    // ...and the caller expects the lock held when this returns, on every one of
    // the dozen error paths below as well as the happy one. Remembering to
    // re-take it at each `return` is precisely the thing that gets forgotten at
    // the thirteenth, so it is a destructor's job instead.
    struct Relock {
        std::unique_lock<std::shared_mutex>& held;
        ~Relock() {
            if (!held.owns_lock()) {
                held.lock();
            }
        }
    } const relock{lock};

    std::vector<std::unique_ptr<EntrySource>> sources;
    sources.reserve(work.inputs.size() + work.overlaps.size());

    // Newest first: the inputs come from the shallower level and so are newer
    // than everything they overlap below. Within L0 they are already ordered by
    // descending id. That ordering is all the merge needs to break ties
    // correctly -- there is no version number anywhere in this.
    for (const std::shared_ptr<const SSTable>& table : work.inputs) {
        auto source = TableSource::create(table, {}, {});
        if (!source.is_ok()) {
            return source.status();
        }
        sources.push_back(source.take());
        LsmStats::bump(stats_.compaction_bytes_read, table->file_size());
    }
    for (const std::shared_ptr<const SSTable>& table : work.overlaps) {
        auto source = TableSource::create(table, {}, {});
        if (!source.is_ok()) {
            return source.status();
        }
        sources.push_back(source.take());
        LsmStats::bump(stats_.compaction_bytes_read, table->file_size());
    }

    // Emit, not Skip: a tombstone this compaction is not allowed to drop has to
    // be written into the output, or the value it buries reappears from below.
    MergingIterator merged{std::move(sources), Tombstones::Emit};

    std::vector<FileId> output_ids;
    std::optional<SSTableBuilder> builder;
    FileId building = 0;

    const auto start_output = [&]() -> Status {
        building = next_id_++;
        auto created = SSTableBuilder::create(dir_ / temp_name(sst_file_name(building)),
                                              options_.block_size, options_.bits_per_key);
        if (!created.is_ok()) {
            return created.status();
        }
        builder.emplace(created.take());
        return {};
    };

    const auto finish_output = [&]() -> Status {
        if (!builder.has_value()) {
            return {};
        }
        // A compaction that drops every key it read produces nothing. Writing an
        // empty table would be legal and pointless, so the temp is discarded --
        // note the reset() first, because the file must be closed before Windows
        // will unlink it.
        if (builder->entry_count() == 0) {
            const std::filesystem::path temp = builder->path();
            builder.reset();
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return {};
        }
        KVSTORE_RETURN_IF_ERROR(builder->finish());
        LsmStats::bump(stats_.compaction_bytes_written, builder->file_size());
        output_ids.push_back(building);
        builder.reset();
        return {};
    };

    KVSTORE_RETURN_IF_ERROR(merged.seek_to_first());
    while (merged.valid()) {
        if (!merged.tombstone() || !work.drop_tombstones) {
            if (!builder.has_value()) {
                KVSTORE_RETURN_IF_ERROR(start_output());
            }
            KVSTORE_RETURN_IF_ERROR(
                builder->add(merged.key(), merged.value(), merged.tombstone()));

            // Split, so one merge yields several bounded files rather than one
            // enormous one -- which is what keeps the *next* compaction of this
            // level bounded too.
            if (builder->file_size() >= options_.target_table_size) {
                KVSTORE_RETURN_IF_ERROR(finish_output());
            }
        }
        KVSTORE_RETURN_IF_ERROR(merged.next());
    }
    KVSTORE_RETURN_IF_ERROR(finish_output());
    KVSTORE_RETURN_IF_ERROR(merged.status());

    if (options_.fail_at == LsmFailPoint::AfterWritingTable) {
        return {};
    }

    for (const FileId id : output_ids) {
        const std::string name = sst_file_name(id);
        if (platform::rename_file(dir_ / temp_name(name), dir_ / name) != 0) {
            return Status::io_error("rename " + temp_name(name) + " to " + name + ": " +
                                    platform::last_error());
        }
    }
    KVSTORE_RETURN_IF_ERROR(sync_dir());

    if (options_.fail_at == LsmFailPoint::AfterInstallingTable) {
        return {};
    }

    // Build the level set the commit will describe: the inputs gone from their
    // level, the overlaps gone from the one below, and the outputs in their
    // place, sorted by min_key so the level stays binary-searchable.
    const std::size_t output_level = work.level + 1;
    Levels next = levels_;
    if (next.size() <= output_level) {
        next.resize(output_level + 1);
    }

    std::vector<FileId> retired;
    retired.reserve(work.inputs.size() + work.overlaps.size());
    for (const std::shared_ptr<const SSTable>& table : work.inputs) {
        retired.push_back(table->id());
    }
    for (const std::shared_ptr<const SSTable>& table : work.overlaps) {
        retired.push_back(table->id());
    }
    const auto is_retired = [&retired](const std::shared_ptr<const SSTable>& table) {
        return std::find(retired.begin(), retired.end(), table->id()) != retired.end();
    };

    auto& source_level = next[work.level];
    source_level.erase(std::remove_if(source_level.begin(), source_level.end(), is_retired),
                       source_level.end());
    auto& target_level = next[output_level];
    target_level.erase(std::remove_if(target_level.begin(), target_level.end(), is_retired),
                       target_level.end());

    for (const FileId id : output_ids) {
        auto opened = SSTable::open(dir_ / sst_file_name(id), id, &stats_,
                                    options_.read_mode, options_.bloom_enabled);
        if (!opened.is_ok()) {
            return opened.status();
        }
        target_level.push_back(opened.take());
    }
    std::sort(target_level.begin(), target_level.end(),
              [](const std::shared_ptr<const SSTable>& a, const std::shared_ptr<const SSTable>& b) {
                  return a->min_key() < b->min_key();
              });

    // Trailing empty levels are noise in the manifest and in every loop that
    // walks the tree.
    while (!next.empty() && next.back().empty()) {
        next.pop_back();
    }

    KVSTORE_RETURN_IF_ERROR(commit_manifest(next, next_id_.load()));

    // The swap, and everything else that touches shared state, under the lock.
    //
    // compact_pointer_ belongs in here too, which is easy to miss: it is only
    // ever written by this thread, but pick_compaction() *reads* it from under
    // the lock, so writing it outside is a race even though only one writer
    // exists. ThreadSanitizer said so; nothing else would have.
    lock.lock();
    levels_ = std::move(next);
    publish_locked();

    // Remember where this level got to, so the next compaction of it starts
    // after here rather than back at the beginning.
    if (compact_pointer_.size() <= work.level) {
        compact_pointer_.resize(work.level + 1);
    }
    compact_pointer_[work.level] = work.end;
    lock.unlock();

    if (installed != nullptr) {
        *installed = true;
    }

    LsmStats::bump(stats_.compactions);

    if (options_.fail_at == LsmFailPoint::AfterManifestCommit) {
        return {};  // Committed, but the inputs are never unlinked.
    }

    // Past the commit, so the inputs are unreachable: nothing references them,
    // and recovery would sweep them anyway. They are *not* deleted here, because
    // a reader that started before the swap may still be inside one.
    //
    // Instead each is marked, and the file goes when the last reference to the
    // table goes -- which is the old Version's, once the last reader holding it
    // finishes. The SSTable destructor unmaps, closes, then unlinks, in that
    // order, because Windows will not remove a file with a live mapping or an
    // open handle.
    for (const std::shared_ptr<const SSTable>& table : work.inputs) {
        table->mark_obsolete();
        LsmStats::bump(stats_.tables_obsoleted);
    }
    for (const std::shared_ptr<const SSTable>& table : work.overlaps) {
        table->mark_obsolete();
        LsmStats::bump(stats_.tables_obsoleted);
    }
    work.inputs.clear();
    work.overlaps.clear();

    return sync_dir();
}

}  // namespace kvstore
