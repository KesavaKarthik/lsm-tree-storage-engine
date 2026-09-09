#include "kvstore/wal.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "kvstore/record.hpp"
#include "log_scan.hpp"

namespace kvstore {

namespace {

std::uint64_t now_nanos() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

Result<Wal> Wal::open(const std::filesystem::path& path, SyncMode mode) {
    auto file = LogFile::open(path, mode);
    if (!file.is_ok()) {
        return file.status();
    }
    return Wal{file.take()};
}

Status Wal::append(std::string_view key, std::string_view value, bool tombstone) {
    const std::vector<std::uint8_t> bytes =
        record::encode(key, tombstone ? std::string_view{} : value, now_nanos(), tombstone);

    auto offset = file_.append(bytes);
    if (!offset.is_ok()) {
        return offset.status();
    }
    return {};
}

Result<std::uint64_t> Wal::replay_into(Memtable* into) {
    if (into == nullptr) {
        return Status::invalid_argument("memtable out-parameter must not be null");
    }

    std::uint64_t replayed = 0;
    auto good_end = scan_records(
        file_, [&](std::uint64_t, const record::Record& rec, std::span<const std::uint8_t>) {
            // In order, so a later write for the same key overwrites an earlier
            // one. This is the whole of replay: the memtable's own overwrite
            // semantics do the reconciling, and no timestamp comparison is
            // needed because the file *is* the order.
            if (rec.is_tombstone) {
                into->remove(rec.key);
            } else {
                into->put(rec.key, rec.value);
            }
            ++replayed;
            return Status::ok();
        });
    if (!good_end.is_ok()) {
        return good_end.status();
    }

    // Cut the torn tail. Not optional housekeeping: the next append() would
    // otherwise land *after* the partial record, leaving a permanent hole that
    // every future replay would stop at -- silently discarding everything
    // written from here on.
    if (*good_end != file_.size()) {
        KVSTORE_RETURN_IF_ERROR(file_.truncate(*good_end));
    }

    return replayed;
}

Status Wal::sync() { return file_.sync(); }

Status Wal::close() { return file_.close(); }

}  // namespace kvstore
