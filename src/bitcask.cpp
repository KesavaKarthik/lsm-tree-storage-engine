#include "kvstore/bitcask.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

#include "kvstore/hint.hpp"
#include "kvstore/record.hpp"
#include "encoding.hpp"
#include "log_scan.hpp"
#include "platform_file.hpp"

namespace kvstore {
namespace {

std::uint64_t now_nanos() {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

// Refuse a record whose total size wouldn't fit the uint32_t the index and
// read_at carry it in (record::kMaxRecordSize). Checking the total subsumes a
// per-field check, since neither part can exceed the whole. Subtracting the
// header from the limit rather than adding it to the payload keeps the
// arithmetic clear of overflow.
Status check_record_fits(const std::string& key, const std::string& value) {
    const std::uint64_t payload =
        static_cast<std::uint64_t>(key.size()) + static_cast<std::uint64_t>(value.size());
    if (payload > record::kMaxRecordSize - record::kHeaderSize) {
        return Status::invalid_argument("record exceeds the maximum record size");
    }
    return Status::ok();
}

}  // namespace

Result<std::unique_ptr<Bitcask>> Bitcask::open(const std::filesystem::path& directory,
                                               const BitcaskOptions& options) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    // create_directories reports "already existed" as false with no error, so
    // only a real error matters here.
    if (ec) {
        return Status::io_error("create directory " + directory.string() + ": " + ec.message());
    }

    // Private constructor, so make_unique can't reach it.
    std::unique_ptr<Bitcask> store{new Bitcask{directory, options}};

    Status s = store->recover();
    if (!s.is_ok()) {
        return s;
    }
    return store;
}

// ---------------------------------------------------------------------------
// Recovery
//
// Replay every data file front to back, in ascending file id order, applying
// each record to the index in the order it was written. That reconstructs the
// exact pre-crash index, and the reason is that the files *are* the history:
// the index was only ever changed by these same operations, in this same order.
// Replaying them is re-running that history. Later records for a key overwrite
// earlier ones simply because they are applied later, which is why "last write
// wins" needs no timestamp comparison here.
//
// File id order is written-order because ids only ever go up: rotation takes
// the next one, and compaction reuses an id it already owns rather than
// inventing a higher one. Keeping that true is what lets this loop stay as
// simple as it was when there was one file.
//
// Tombstones erase during replay for the same reason. A delete cannot remove
// the earlier record from an append-only file, so both are on disk and the
// tombstone is later; applying it in order leaves the key absent, exactly as it
// was before the crash. Skipping tombstones would resurrect deleted keys on
// every restart.
// ---------------------------------------------------------------------------
Status Bitcask::recover() {
    // Before deciding what the database contains, throw away what it doesn't:
    // an interrupted compaction leaves a .tmp nothing ever pointed at.
    KVSTORE_RETURN_IF_ERROR(sweep_temp_files());

    std::error_code ec;
    std::vector<FileId> ids;
    std::vector<FileId> hint_ids;

    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // Anything we don't recognise is ignored rather than guessed at -- in
        // particular a leftover .log.tmp, which is an interrupted compaction's
        // output and is not part of the database.
        if (const auto id = parse_log_file_id(name)) {
            ids.push_back(*id);
        } else if (const auto hint_id = parse_hint_file_id(name)) {
            hint_ids.push_back(*hint_id);
        }
    }
    if (ec) {
        return Status::io_error("scan directory " + dir_.string() + ": " + ec.message());
    }

    // Numerically, never lexicographically: the zero padding makes the two
    // agree today and would stop doing so past six digits.
    std::sort(ids.begin(), ids.end());

    // A hint whose data file is gone describes nothing. Compaction deletes the
    // two together but not atomically, so a crash in between can leave one
    // behind. Harmless -- replay_hint would reject it on the file-size check --
    // but there is no reason to keep it.
    for (const FileId hint_id : hint_ids) {
        if (!std::binary_search(ids.begin(), ids.end(), hint_id)) {
            std::filesystem::remove(dir_ / hint_file_name(hint_id), ec);
        }
    }

    if (ids.empty()) {
        ids.push_back(kFirstFileId);  // A fresh database. Creating it is open()'s job below.
    }

    for (std::size_t i = 0; i < ids.size(); ++i) {
        const FileId id = ids[i];
        auto file = LogFile::open(dir_ / log_file_name(id), options_.sync_mode);
        if (!file.is_ok()) {
            return file.status();
        }

        const auto [it, inserted] = files_.try_emplace(id, file.take());
        (void)inserted;

        // The highest id is the one we will append to. Everything below it is
        // sealed.
        const bool is_active = (i + 1 == ids.size());

        // A sealed file may have a hint beside it, which rebuilds the same
        // index without reading the values. Never for the active file: we are
        // about to append to it, so its size is about to stop matching anything
        // a hint could have recorded -- and no hint is ever written for it.
        if (!is_active) {
            auto used_hint = replay_hint(id, it->second);
            if (!used_hint.is_ok()) {
                return used_hint.status();
            }
            if (*used_hint) {
                continue;
            }
        }

        KVSTORE_RETURN_IF_ERROR(replay_file(id, it->second, is_active));
    }

    active_id_ = ids.back();
    return Status::ok();
}

// Replays one file, and decides what a bad record means -- which depends
// entirely on whether the file was still being written to.
//
// **Active file.** The only writer is append(), which never modifies existing
// bytes, so record N cannot be damaged by anything that happened after it. The
// sole way to get a bad record is for the machine to have died partway through
// writing the last one. Everything before it is complete and fsynced. So the
// first bad record is the boundary: stop, keep what came before, and truncate
// the remainder so the next append doesn't build on top of garbage. If we
// merely stopped reading, the next put would write a valid record *after* the
// garbage, and the following recovery would stop at that garbage and silently
// lose everything beyond it.
//
// **Sealed file.** A sealed file was written completely and fsynced before the
// engine moved on, so a crash cannot explain a bad record in it -- damaged
// media can. Truncating would be the wrong response twice over: it would
// destroy live data on disk in reaction to a read error, and it would do it
// silently. So we refuse to open and say which file and where. That leaves a
// single bad sector able to make the database unopenable, which is a real cost;
// it is the right default for something whose worst failure mode is quietly
// returning wrong data, and a repair tool is the answer if it ever bites.
Status Bitcask::replay_file(FileId id, LogFile& file, bool is_active) {
    auto good_end = scan_records(file, [&](std::uint64_t offset, const record::Record& rec,
                                           std::span<const std::uint8_t>) {
        if (rec.is_tombstone) {
            if (const auto buried = index_.erase(rec.key)) {
                mark_dead(buried->file_id, buried->size);
            }
            // The tombstone itself is dead weight too: it carries no live data
            // and compaction will drop it.
            mark_dead(id, rec.total_size);
        } else {
            const ValuePointer pointer{.offset = offset,
                                       .timestamp = rec.timestamp,
                                       .file_id = id,
                                       .size = rec.total_size};
            if (const auto displaced = index_.put(rec.key, pointer)) {
                mark_dead(displaced->file_id, displaced->size);
            }
        }
        return Status::ok();
    });
    if (!good_end.is_ok()) {
        return good_end.status();
    }

    if (*good_end == file.size()) {
        return Status::ok();
    }

    if (!is_active) {
        return Status::corruption("sealed data file " + file.path().string() +
                                  " fails to verify at offset " + std::to_string(*good_end) +
                                  "; it was written and fsynced in full, so this is damage, "
                                  "not a torn write");
    }
    return file.truncate(*good_end);
}

// Rebuilds one sealed file's index entries from its hint sidecar.
//
// Every rejection path returns `false`, not an error. A hint is a cache: absent,
// short, stale, or damaged all mean the same thing -- read the data file
// instead. Making any of them fatal would let a corrupt sidecar take down a
// database whose actual data is fine.
//
// The whole hint is validated *before* a single entry reaches the index. A
// partial application followed by a fallback scan would leave entries from a
// file we had just decided not to trust, for keys that file may not even
// contain.
//
// What this trades away: replaying from a hint means the data file's crcs are
// never checked at startup. That is the entire speedup -- we are deliberately
// not reading the bytes. Rot in a compacted file is therefore caught on the
// first get() of the affected key rather than at open, which still means it is
// caught before anyone is handed a wrong answer, just later.
Result<bool> Bitcask::replay_hint(FileId id, const LogFile& data_file) {
    const std::filesystem::path path = dir_ / hint_file_name(id);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }

    auto hint_file = LogFile::open(path, SyncMode::Never);
    if (!hint_file.is_ok()) {
        return false;
    }

    const std::uint64_t size = hint_file->size();
    if (size < hint::kFooterSize) {
        return false;  // Too small to even hold the footer that commits it.
    }
    const std::uint64_t entries_end = size - hint::kFooterSize;

    auto footer_bytes =
        hint_file->read_at(entries_end, static_cast<std::uint32_t>(hint::kFooterSize));
    if (!footer_bytes.is_ok()) {
        return false;
    }
    auto footer = hint::decode_footer(*footer_bytes);
    if (!footer.is_ok()) {
        return false;  // No valid footer: the hint was never finished.
    }

    // The hint describes a data file of a particular length. If the real one is
    // a different length, this hint belongs to a file that no longer exists and
    // its offsets point at unrelated bytes.
    if (footer->data_file_size != data_file.size()) {
        return false;
    }

    // Bound the declared count against the file before reserving for it -- the
    // same discipline the record path uses, and for the same reason: this
    // number came off disk and nothing has vouched for it yet. Every entry is
    // at least kEntryHeaderSize bytes, so this is a tight ceiling.
    if (footer->entry_count > entries_end / hint::kEntryHeaderSize) {
        return false;
    }

    std::vector<hint::Entry> entries;
    entries.reserve(static_cast<std::size_t>(footer->entry_count));

    std::uint64_t offset = 0;
    while (offset < entries_end) {
        if (offset + hint::kEntryHeaderSize > entries_end) {
            return false;  // A partial entry: the hint is malformed.
        }

        auto header_bytes =
            hint_file->read_at(offset, static_cast<std::uint32_t>(hint::kEntryHeaderSize));
        if (!header_bytes.is_ok()) {
            return false;
        }
        // Only the key length is needed to size the real read; decode_entry
        // does the rest once we have the whole entry.
        const std::uint64_t key_size = encoding::get_u32(*header_bytes, 24);
        const std::uint64_t total = hint::kEntryHeaderSize + key_size;
        if (offset + total > entries_end) {
            return false;
        }

        auto entry_bytes = hint_file->read_at(offset, static_cast<std::uint32_t>(total));
        if (!entry_bytes.is_ok()) {
            return false;
        }
        auto entry = hint::decode_entry(*entry_bytes);
        if (!entry.is_ok()) {
            return false;
        }

        // The entry must address a record that fits inside the data file. An
        // offset past the end would send get() to read bytes that aren't there.
        if (entry->record_offset + entry->record_size > data_file.size()) {
            return false;
        }

        entries.push_back(entry.take());
        offset += total;
    }

    // Both totals have to agree, or we read something other than what was
    // written -- and the footer is the only witness to what "complete" means.
    if (offset != entries_end || entries.size() != footer->entry_count) {
        return false;
    }

    // Validated in full; only now does any of it become the index.
    for (const auto& entry : entries) {
        const ValuePointer pointer{.offset = entry.record_offset,
                                   .timestamp = entry.timestamp,
                                   .file_id = id,
                                   .size = entry.record_size};
        if (const auto displaced = index_.put(entry.key, pointer)) {
            mark_dead(displaced->file_id, displaced->size);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------

Status Bitcask::rotate() {
    // Seal the outgoing file. Under SyncMode::Never nothing has been forced to
    // the device yet, and we are about to stop touching this file forever --
    // this is the last chance to make it durable, and every reader from here on
    // treats it as complete.
    KVSTORE_RETURN_IF_ERROR(active_file().sync());

    if (active_id_ == std::numeric_limits<FileId>::max()) {
        return Status::io_error("file ids exhausted");
    }
    const FileId next_id = active_id_ + 1;

    auto file = LogFile::open(dir_ / log_file_name(next_id), options_.sync_mode);
    if (!file.is_ok()) {
        return file.status();
    }
    files_.try_emplace(next_id, file.take());
    active_id_ = next_id;

    // The new file's *name* has to be durable too, not just its (empty)
    // contents. Cheap: rotation happens once per max_file_size bytes written.
    if (platform::sync_directory(dir_) != 0) {
        return Status::io_error("fsync directory " + dir_.string() + ": " +
                                platform::last_error());
    }
    return Status::ok();
}

Status Bitcask::rotate_if_needed(std::size_t incoming_bytes) {
    const std::uint64_t current = active_file().size();

    // An empty file never rotates. Without this, a record larger than the
    // threshold would rotate forever and never be written -- instead it gets a
    // file to itself, oversized and immutable, which nothing minds.
    if (current == 0) {
        return Status::ok();
    }
    if (current + incoming_bytes <= options_.max_file_size) {
        return Status::ok();
    }

    // Checked *before* the append, never after. A record must lie entirely
    // within one file: the framing has no way to continue a record across a
    // boundary, and the index addresses a record by a single {file, offset}.
    return rotate();
}

// ---------------------------------------------------------------------------
// The KVStore contract
// ---------------------------------------------------------------------------

Status Bitcask::put(const std::string& key, const std::string& value) {
    // Same rule as MemoryStore, and here it has teeth: key_size is a header
    // field, so a zero-length key would be indistinguishable from a corrupt
    // header during recovery.
    if (key.empty()) {
        return Status::invalid_argument("key must not be empty");
    }
    KVSTORE_RETURN_IF_ERROR(check_record_fits(key, value));

    // One timestamp for both the record and the index entry -- they describe
    // the same write and must not drift apart.
    const std::uint64_t timestamp = now_nanos();
    const std::vector<std::uint8_t> bytes = record::encode(key, value, timestamp, false);

    KVSTORE_RETURN_IF_ERROR(rotate_if_needed(bytes.size()));

    auto offset = active_file().append(bytes);
    if (!offset.is_ok()) {
        return offset.status();
    }

    // Index only after the append succeeded. Doing it first would leave the
    // index pointing at an offset that holds nothing if the write failed.
    // The size cast is safe because check_record_fits bounded the total above.
    const ValuePointer pointer{.offset = *offset,
                               .timestamp = timestamp,
                               .file_id = active_id_,
                               .size = static_cast<std::uint32_t>(bytes.size())};
    if (const auto displaced = index_.put(key, pointer)) {
        // The record we just superseded is still on disk and now unreachable.
        mark_dead(displaced->file_id, displaced->size);
    }
    return Status::ok();
}

Status Bitcask::get(const std::string& key, std::string* value) {
    if (value == nullptr) {
        return Status::invalid_argument("value out-parameter must not be null");
    }

    const ValuePointer* pointer = index_.get(key);
    if (pointer == nullptr) {
        return Status::not_found("no such key: " + key);
    }

    const auto file = files_.find(pointer->file_id);
    if (file == files_.end()) {
        // The index named a file we don't have open. Only a bug can do this --
        // recovery opens every file it replays, and compaction updates the
        // index and the file set together -- so say so rather than guess.
        return Status::corruption("index points at file " +
                                  std::to_string(pointer->file_id) + ", which is not open");
    }

    auto bytes = file->second.read_at(pointer->offset, pointer->size);
    if (!bytes.is_ok()) {
        return bytes.status();
    }

    // Verified on every read, not just at recovery. The bytes could have rotted
    // on the platter since we wrote them, and returning silently wrong data is
    // the worst thing a storage engine can do. It matters more now that hint
    // files let recovery skip reading data files at all: this is the only place
    // a compacted file's crcs are ever checked.
    auto rec = record::decode(*bytes);
    if (!rec.is_ok()) {
        return rec.status();
    }

    // The record we landed on should be the one we asked for. If it isn't, the
    // index and the file disagree -- a bug or a damaged file, either way not
    // something to paper over by returning whatever we found.
    if (rec->key != key) {
        return Status::corruption("index points at a record for a different key");
    }
    if (rec->is_tombstone) {
        return Status::corruption("index points at a tombstone");
    }

    *value = std::move(rec->value);
    return Status::ok();
}

Status Bitcask::remove(const std::string& key) {
    if (key.empty()) {
        return Status::invalid_argument("key must not be empty");
    }
    KVSTORE_RETURN_IF_ERROR(check_record_fits(key, {}));

    // The tombstone is appended whether or not the key is live. We could skip
    // it when the index says the key is absent, but that shortcut is only valid
    // while the index is complete and authoritative -- which stops being true
    // once there are multiple files to merge. Keeping the log a full record of
    // what was asked for costs a few bytes and avoids a subtle rule to remember.
    const std::vector<std::uint8_t> bytes = record::encode(key, {}, now_nanos(), true);

    KVSTORE_RETURN_IF_ERROR(rotate_if_needed(bytes.size()));

    auto offset = active_file().append(bytes);
    if (!offset.is_ok()) {
        return offset.status();
    }

    if (const auto buried = index_.erase(key)) {
        mark_dead(buried->file_id, buried->size);
    }
    mark_dead(active_id_, static_cast<std::uint32_t>(bytes.size()));

    // Ok even if nothing was there: see memory_store.cpp for why delete is
    // idempotent.
    return Status::ok();
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

Status Bitcask::sync() { return active_file().sync(); }

std::uint64_t Bitcask::active_file_size() const { return active_file().size(); }

std::uint64_t Bitcask::total_disk_size() const {
    std::uint64_t total = 0;
    for (const auto& [id, file] : files_) {
        total += file.size();
    }
    return total;
}

std::uint64_t Bitcask::reclaimable_bytes() const {
    std::uint64_t total = 0;
    for (const auto& [id, bytes] : dead_bytes_) {
        total += bytes;
    }
    return total;
}

}  // namespace kvstore
