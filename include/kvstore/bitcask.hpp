#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kvstore/file_names.hpp"
#include "kvstore/index.hpp"
#include "kvstore/kvstore.hpp"
#include "kvstore/log_file.hpp"
#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// Where compact() should stop, as if the process had died there.
//
// Test-only, and the reason it lives in the production options rather than in
// the test file is that the states worth testing are the ones compact()
// actually produces, in the order it produces them. A test that stages those
// directories by hand tests the sequence the test author imagined, which is
// exactly the thing a sequencing bug would also have got wrong. This is the
// concrete form of the fault-injection seam the KVStore interface was drawn for.
enum class CompactionFailPoint {
    None,
    AfterWritingTemp,       // Output written and fsynced, nothing installed yet.
    AfterInstallingOutput,  // Merged file renamed into place, inputs still there.
    MidInputDelete,         // Some inputs deleted, some not.
};

struct BitcaskOptions {
    // Always by default: a store that loses acknowledged writes on a power cut
    // should be something you opt into, not something you get by forgetting.
    SyncMode sync_mode = SyncMode::Always;

    // Roll the active file once appending would push it past this. Records
    // never straddle a boundary, so a record larger than this gets a file of
    // its own rather than being split.
    //
    // 64MiB because every data file stays open for reading: at this size a
    // 64GiB database holds 1024 descriptors open, which is exactly the default
    // Linux soft limit. Raising the threshold is the cheap fix; an LRU
    // descriptor cache is the real one, and neither is needed yet.
    std::uint64_t max_file_size = 64ull << 20;

    // Test-only; see CompactionFailPoint.
    CompactionFailPoint fail_at = CompactionFailPoint::None;
};

// Bitcask: append-only data files plus an in-memory index over them.
//
//   put    -- encode a record, append it to the active file, point the index at
//             the new {file, offset}. The old record stays on disk, dead but
//             harmless, until compaction.
//   get    -- index lookup, one positional read, verify crc, return the value.
//   remove -- append a tombstone, drop the key from the index.
//
// Nothing is ever overwritten in place. That is what makes a crash survivable:
// the only damage a crash can do is leave a partial record at the very end of
// the active file, and a partial record fails its crc and gets cut off at
// startup. Everything before it was already complete and already fsynced.
//
// One file is *active* (the highest id, the only one appended to); the rest are
// sealed and immutable. That distinction is what makes compaction possible at
// all -- you cannot merge a file someone is still writing to.
//
// Single-threaded. Not safe to share across threads, and not safe to open the
// same directory twice at once -- both would append at their own idea of the
// end offset. A lock file is a later phase.
class Bitcask final : public KVStore {
public:
    // Opens (creating if needed) the directory and replays every data file in
    // it. Fails with IOError if the directory or a file can't be opened, and
    // with Corruption if a sealed file fails its crc (see recover()).
    //
    // A factory rather than a constructor because opening does real work that
    // can fail, and Status is how this codebase reports failure -- a
    // constructor's only channel would be an exception.
    [[nodiscard]] static Result<std::unique_ptr<Bitcask>> open(
        const std::filesystem::path& directory, const BitcaskOptions& options = {});

    ~Bitcask() override = default;

    Status put(const std::string& key, const std::string& value) override;
    Status get(const std::string& key, std::string* value) override;
    Status remove(const std::string& key) override;

    // Force a flush of the active file. Meaningful under SyncMode::Never; a
    // no-op cost otherwise. Sealed files were fsynced before they were sealed.
    [[nodiscard]] Status sync();

    // Merge every sealed file into one, keeping only the record each live key
    // points at and dropping superseded values and tombstones. Reclaims exactly
    // reclaimable_bytes(), and is what stops an append-only store from growing
    // without bound relative to the data actually in it.
    //
    // Synchronous and manual: this can move every byte in the database, so when
    // it happens should be the caller's decision rather than a surprise inside
    // some unlucky put(). Automatic triggering belongs with leveled compaction.
    //
    // Crash-safe. It writes to a temp file, fsyncs it, and renames it into
    // place, so a crash at any point leaves a database that recovers to the
    // same contents -- see the sequence in compaction.cpp, where the ordering
    // *is* the argument.
    [[nodiscard]] Status compact();

    // --- Diagnostics. Not part of the KVStore contract. ---------------------

    // Live keys.
    [[nodiscard]] std::size_t key_count() const noexcept { return index_.size(); }

    // Size of the file currently being appended to.
    [[nodiscard]] std::uint64_t active_file_size() const;

    // Every data file added up -- what the database actually costs on disk.
    [[nodiscard]] std::uint64_t total_disk_size() const;

    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }

    [[nodiscard]] FileId active_file_id() const noexcept { return active_id_; }

    // Bytes held by records no live key points at: superseded values, and the
    // tombstones that buried deleted ones. This is space amplification made
    // countable, and it is what compaction would give back.
    [[nodiscard]] std::uint64_t reclaimable_bytes() const;

    // Every live index entry. Lets a test assert that two recovery paths
    // rebuild *exactly* the same index, not merely the same values.
    [[nodiscard]] std::vector<std::pair<std::string, ValuePointer>> index_snapshot() const {
        return index_.snapshot();
    }

private:
    Bitcask(std::filesystem::path directory, const BitcaskOptions& options)
        : dir_(std::move(directory)), options_(options) {}

    // Opens every data file in the directory and replays them in id order.
    [[nodiscard]] Status recover();

    // Replays one file into the index. `is_active` selects what happens at a
    // record that doesn't verify -- see the comment on the definition, it is
    // the substance of the difference between a torn tail and a damaged file.
    [[nodiscard]] Status replay_file(FileId id, LogFile& file, bool is_active);

    // Rebuilds the index for one sealed file from its hint sidecar instead of
    // reading the file itself. Returns false -- never an error -- if there is
    // no usable hint, in which case the caller must fall back to replay_file.
    [[nodiscard]] Result<bool> replay_hint(FileId id, const LogFile& data_file);

    // Seals the active file and starts the next one.
    [[nodiscard]] Status rotate();

    // Rotates first if appending `incoming_bytes` would overflow the threshold.
    [[nodiscard]] Status rotate_if_needed(std::size_t incoming_bytes);

    // Deletes any half-written file an interrupted compaction left behind.
    [[nodiscard]] Status sweep_temp_files();

    // fsync the directory itself, committing a name that was just created,
    // renamed, or removed.
    [[nodiscard]] Status sync_dir() const;

    // The only file that may be appended to.
    [[nodiscard]] LogFile& active_file() { return files_.at(active_id_); }
    [[nodiscard]] const LogFile& active_file() const { return files_.at(active_id_); }

    // Records that `size` bytes in `id` are no longer reachable from the index.
    void mark_dead(FileId id, std::uint32_t size) { dead_bytes_[id] += size; }

    std::filesystem::path dir_;
    BitcaskOptions options_;

    // Ordered, because both recovery and compaction depend on visiting files
    // oldest-id-first: file id order *is* the order records were written, and
    // that is the engine's only authority on which of two records is newer.
    std::map<FileId, LogFile> files_;
    FileId active_id_ = 0;

    Index index_;

    // Per file, so a future compaction policy can pick the worst offender
    // rather than always merging everything.
    std::map<FileId, std::uint64_t> dead_bytes_;
};

}  // namespace kvstore
