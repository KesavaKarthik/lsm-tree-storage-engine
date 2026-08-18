#include <algorithm>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/hint.hpp"
#include "kvstore/record.hpp"
#include "log_scan.hpp"
#include "platform_file.hpp"

// ---------------------------------------------------------------------------
// Compaction
//
// An append-only store never reclaims anything on its own: every overwrite
// leaves the value it replaced on disk, and every delete *adds* a tombstone on
// top of the record it buries. Compaction is what gives that space back. It
// reads the sealed files, keeps only the record each live key actually points
// at, and writes them into one new file -- dropping superseded values and
// tombstones alike.
//
// The merge itself is easy. Doing it without ever opening a window where a
// crash loses data or resurrects a deleted key is the whole problem, and it is
// solved entirely by the *order* of the steps below.
//
//
// THE INVARIANT
//
// At every instant, from the moment compaction starts to the moment it
// finishes, the files present in the directory -- replayed in ascending id
// order, which is what recover() does -- must rebuild the correct index.
//
// That is the property being maintained, and everything else follows from it.
//
//
//   1. THE MERGED FILE TAKES THE **LOWEST** INPUT ID.
//
//      The merged file is a snapshot of the live set: for every key, the record
//      that is current right now. Putting it at the oldest id makes it a *base
//      layer* that every surviving original replays on top of and may override.
//      That is what keeps every intermediate state correct, because those
//      originals are the real history and they get the last word.
//
//      The obvious alternative -- give the merged file the *highest* input id,
//      so it replays last and wins -- is wrong, and wrong in the worst way. Say
//      key K was put in file 2 and deleted by a tombstone in file 5, and the
//      merge drops both. Installing the merged file over id 5 destroys that
//      tombstone while the put in file 2 is still on disk. Replaying 2 then the
//      merged 5 finds the bare put with nothing left to shadow it, and **K
//      comes back from the dead**. That was the original design here, and it
//      shipped as far as the test suite: switching this back to inputs.back()
//      fails CompactionCrash.AfterTheOutputIsInstalled... and ...ADeletedKey-
//      DoesNotComeBackWhenTheMergedFileReplacesItsTombstone, and nothing else.
//
//      Note the bug is only visible if the crash lands between installing the
//      output and deleting the inputs. Run compaction to completion and the
//      resurrected record is deleted along with its file, so the final state is
//      correct and the flaw hides. That is what the fail points are for.
//
//      At the lowest id the same case is fine: merged 1 says nothing about K,
//      file 2 puts it, file 5's tombstone -- still there -- removes it again.
//      Nothing the merged file drops can escape, because everything that
//      shadowed it is still present and still replays afterwards.
//
//      A consequence worth naming: file id order is no longer strictly *write*
//      order, since the merged file at id 1 holds records first written into
//      file 5. What it is instead is *replay* order, which is the property
//      recovery actually needs and the only one it ever relied on.
//
//   2. INSTALL THE OUTPUT BEFORE DELETING ANY INPUT.
//      A crash before the rename leaves the original files untouched and a
//      stray .tmp that nothing points at, swept away at the next open. A crash
//      after it leaves the base layer plus the full history on top. Both
//      states rebuild the same index.
//
//   3. DELETE THE INPUTS LOWEST-ID-FIRST.
//      Having deleted inputs 1..k, replay is the merged file followed by the
//      surviving k+1..N. For any key, either its winning record was in a
//      deleted file -- in which case the merged file carries it and nothing
//      later contradicts it -- or it is in a surviving file, which re-asserts
//      it in order. Correct either way.
//
//      Delete in any other order and that breaks. Keep file 2 while deleting
//      file 5, and file 2's *older* record for K replays after the merged
//      file's newer one and silently wins.
//
//      The rule generalises: a surviving original must never be older than
//      something already deleted.
//
//
// WHY A TOMBSTONE MAY BE DROPPED AT ALL
//
// Only because every file older than it is in the same merge. A tombstone's job
// is to shadow records that came before it; once those records are gone from
// the database, it has nothing left to say. Compact every sealed file at once
// and that is guaranteed. **If compaction ever becomes partial -- merging a
// subset of files, as leveled compaction will -- tombstones must be retained
// until the oldest file that could hold the key is included.** That is a
// constraint on Phase 5, not a free choice.
//
//
// WHY TEMP + FSYNC + RENAME IS ATOMIC
//
// rename(2) replaces one directory entry. A reader opening the name sees the
// old file or the new one, never a blend and never nothing -- there is no
// instant at which the name is unresolvable. The fsync beforehand is what makes
// that meaningful: without it the rename could commit a name pointing at bytes
// that never reached the device. And the fsync *afterwards*, on the directory,
// is what makes the rename itself durable -- see platform::sync_directory,
// which is the step that is easiest to leave out and hardest to notice missing.
//
// One nuance worth stating plainly, because it looks like a contradiction: the
// merged output takes over an id that was in use, so it replaces a name that
// was in use. It does not mutate that file. On POSIX the old inode survives for
// as long as anyone holds it open, and rename never touches file contents. On
// Windows we have to close our own descriptor first, because open_read_write
// asks for _SH_DENYNO, which shares read and write access but not delete.
// ---------------------------------------------------------------------------

namespace kvstore {

Status Bitcask::sync_dir() const {
    if (platform::sync_directory(dir_) != 0) {
        return Status::io_error("fsync directory " + dir_.string() + ": " +
                                platform::last_error());
    }
    return Status::ok();
}

Status Bitcask::sweep_temp_files() {
    std::error_code ec;
    bool removed_any = false;

    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (!is_temp_name(entry.path().filename().string())) {
            continue;
        }
        // Safe to delete unconditionally: a .tmp was never renamed into place,
        // so no directory entry and no index entry has ever referred to it. It
        // is compaction's scratch space, and the merge that would have used it
        // did not finish.
        std::filesystem::remove(entry.path(), ec);
        if (ec) {
            return Status::io_error("remove " + entry.path().string() + ": " + ec.message());
        }
        removed_any = true;
    }
    if (ec) {
        return Status::io_error("scan directory " + dir_.string() + ": " + ec.message());
    }

    return removed_any ? sync_dir() : Status::ok();
}

Status Bitcask::compact() {
    // ---- 1. Seal the active file, so everything mergeable is immutable -----
    //
    // You cannot merge a file that is still being appended to: its size is a
    // moving target, and a record could land in it between the scan and the
    // rename. Rotating first also means compaction can reclaim everything
    // written so far rather than leaving the newest writes out.
    if (active_file().size() > 0) {
        KVSTORE_RETURN_IF_ERROR(rotate());
    }

    std::vector<FileId> inputs;
    for (const auto& [id, file] : files_) {
        if (id < active_id_) {
            inputs.push_back(id);
        }
    }
    if (inputs.empty()) {
        return Status::ok();  // Nothing sealed yet.
    }

    // Already compacted: one sealed file with nothing dead in it. Rewriting it
    // would move every byte to produce an identical file.
    const auto dead_it = dead_bytes_.find(inputs.front());
    if (inputs.size() == 1 && (dead_it == dead_bytes_.end() || dead_it->second == 0)) {
        return Status::ok();
    }

    // The lowest input id -- see invariant 1 above. The merged file is a base
    // layer that the surviving originals replay on top of, so it has to go
    // underneath them. Reusing an id we already own (rather than allocating a
    // new one) is also what keeps recovery free of timestamp comparisons: those
    // timestamps come from system_clock, which is neither monotonic nor finely
    // grained enough to arbitrate between two records.
    const FileId out_id = inputs.front();
    const std::filesystem::path out_path = dir_ / log_file_name(out_id);
    const std::filesystem::path temp_path = dir_ / temp_name(log_file_name(out_id));
    const std::filesystem::path hint_path = dir_ / hint_file_name(out_id);
    const std::filesystem::path hint_temp_path = dir_ / temp_name(hint_file_name(out_id));

    // ---- 2. Write the merged output to a temp file ------------------------
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);  // Leftovers from an earlier attempt.
    std::filesystem::remove(hint_temp_path, ec);

    // SyncMode::Never: one fsync at the end, not one per record. Nothing points
    // at this file until the rename, so a partial temp file is not a state
    // anyone can observe -- there is nothing to be durable about yet.
    auto temp = LogFile::open(temp_path, SyncMode::Never);
    if (!temp.is_ok()) {
        return temp.status();
    }
    // The hint is built in the same pass. Every number it needs -- key,
    // timestamp, new offset, size -- is in hand at the moment the record is
    // written, so a second pass over the merged file would only be re-deriving
    // what we already know.
    auto hint_temp = LogFile::open(hint_temp_path, SyncMode::Never);
    if (!hint_temp.is_ok()) {
        return hint_temp.status();
    }
    std::uint64_t hint_entries = 0;

    std::unordered_map<std::string, ValuePointer> staged;

    for (const FileId id : inputs) {
        LogFile& input = files_.at(id);

        auto good_end = scan_records(
            input, [&](std::uint64_t offset, const record::Record& rec,
                       std::span<const std::uint8_t> raw) -> Status {
                // A tombstone is never copied. See the note above on why that
                // is safe only because every older file is in this same merge.
                if (rec.is_tombstone) {
                    return Status::ok();
                }

                // Live iff the index points at exactly this record. The index
                // is already the authority on what is live, so this needs no
                // extra bookkeeping -- and it correctly drops records for keys
                // whose current value lives in the active file, which is not
                // being merged.
                const ValuePointer* live = index_.get(rec.key);
                if (live == nullptr || live->file_id != id || live->offset != offset) {
                    return Status::ok();
                }

                auto new_offset = temp->append(raw);
                if (!new_offset.is_ok()) {
                    return new_offset.status();
                }

                const auto hint_bytes = hint::encode_entry(rec.key, rec.timestamp, *new_offset,
                                                           rec.total_size);
                auto hint_written = hint_temp->append(hint_bytes);
                if (!hint_written.is_ok()) {
                    return hint_written.status();
                }
                ++hint_entries;

                // Staged, not applied: the index must keep pointing at the old
                // files until the new one is actually installed. If the rename
                // below fails, nothing has changed.
                staged[rec.key] = ValuePointer{.offset = *new_offset,
                                               .timestamp = rec.timestamp,
                                               .file_id = out_id,
                                               .size = rec.total_size};
                return Status::ok();
            });
        if (!good_end.is_ok()) {
            return good_end.status();
        }
        if (*good_end != input.size()) {
            // open() refuses to start on a sealed file that doesn't verify, so
            // reaching this means the file rotted while we were running. Stop
            // rather than write a merged file missing whatever came after it.
            return Status::corruption("data file " + input.path().string() +
                                      " stopped verifying at offset " +
                                      std::to_string(*good_end) + " during compaction");
        }
    }

    // The footer goes on last, and that is what makes it a commit record: a
    // hint file that ends without one was interrupted, and a reader can tell.
    // It also records the data file's length, binding the two together.
    {
        const auto footer = hint::encode_footer(temp->size(), hint_entries);
        auto written = hint_temp->append(footer);
        if (!written.is_ok()) {
            return written.status();
        }
    }

    // Now, and only now, is the output worth anything. After this fsync its
    // bytes are on the device; all that is missing is a name.
    KVSTORE_RETURN_IF_ERROR(temp->sync());
    KVSTORE_RETURN_IF_ERROR(hint_temp->sync());
    // Closed because Windows will not rename a file this process holds open --
    // the rename removes the source name, which needs delete access.
    KVSTORE_RETURN_IF_ERROR(temp->close());
    KVSTORE_RETURN_IF_ERROR(hint_temp->close());

    if (options_.fail_at == CompactionFailPoint::AfterWritingTemp) {
        return Status::ok();
    }

    // ---- 3. Install the output over the lowest input id -------------------
    //
    // Drop the stale hint first. A hint describes byte offsets in one specific
    // data file, so it must never outlive the file it describes -- and there is
    // no instant here at which a hint may survive its data file, because the
    // window between removing the old one and installing the new one is a
    // window in which recovery simply falls back to a full scan. A hint is a
    // cache; being absent is always safe, being wrong never is.
    std::filesystem::remove(hint_path, ec);
    KVSTORE_RETURN_IF_ERROR(sync_dir());

    // Close our descriptor to the file we are about to replace: the same
    // Windows sharing rule as above.
    KVSTORE_RETURN_IF_ERROR(files_.at(out_id).close());
    files_.erase(out_id);

    if (platform::rename_file(temp_path, out_path) != 0) {
        const std::string err = platform::last_error();
        // Nothing was lost: the old file is still on disk under its own name
        // and the index still points into it. Reopen it and report the failure.
        if (auto reopened = LogFile::open(out_path, options_.sync_mode); reopened.is_ok()) {
            files_.try_emplace(out_id, reopened.take());
        }
        return Status::io_error("rename " + temp_path.string() + " -> " + out_path.string() +
                                ": " + err);
    }
    // The rename edited the directory. Until this fsync, the edit is only in
    // memory -- the merged file's *contents* are durable but its *name* is not.
    KVSTORE_RETURN_IF_ERROR(sync_dir());

    if (options_.fail_at == CompactionFailPoint::AfterInstallingOutput) {
        return Status::ok();
    }

    // ---- 4. Adopt the new file -------------------------------------------
    auto merged = LogFile::open(out_path, options_.sync_mode);
    if (!merged.is_ok()) {
        return merged.status();
    }
    files_.try_emplace(out_id, merged.take());

    // Safe to apply the staged pointers wholesale: compaction is synchronous
    // and single-threaded, so nothing has touched the index since they were
    // staged, and each key was staged exactly once -- only the file the index
    // pointed at could have staged it.
    for (const auto& [key, pointer] : staged) {
        (void)index_.put(key, pointer);
    }

    // Every input's dead bytes go away with it, and the merged file has none by
    // construction: it holds exactly the records the index points at.
    for (const FileId id : inputs) {
        dead_bytes_.erase(id);
    }

    // The hint goes in only now that the data file it describes is installed.
    // The reverse order would leave a window where a hint describes a file that
    // is not there yet -- and a wrong hint is the one failure mode this format
    // has no defence against short of the size check that would then reject it.
    if (platform::rename_file(hint_temp_path, hint_path) != 0) {
        // Not fatal, and deliberately so: the database is complete and correct
        // without a hint, it will just take longer to open. Losing the sidecar
        // must never fail an operation that succeeded.
        std::filesystem::remove(hint_temp_path, ec);
    }
    KVSTORE_RETURN_IF_ERROR(sync_dir());

    // ---- 5. Delete the inputs, oldest first -------------------------------
    // The ordering that stops deleted keys from resurrecting. See the header
    // comment; there is a test named for it.
    for (const FileId id : inputs) {
        if (id == out_id) {
            continue;  // That name now belongs to the merged file.
        }
        KVSTORE_RETURN_IF_ERROR(files_.at(id).close());
        files_.erase(id);

        std::filesystem::remove(dir_ / log_file_name(id), ec);
        if (ec) {
            return Status::io_error("remove " + log_file_name(id) + ": " + ec.message());
        }
        std::filesystem::remove(dir_ / hint_file_name(id), ec);

        if (options_.fail_at == CompactionFailPoint::MidInputDelete) {
            return sync_dir();
        }
    }

    return sync_dir();
}

}  // namespace kvstore
