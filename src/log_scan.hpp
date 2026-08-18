#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "kvstore/log_file.hpp"
#include "kvstore/record.hpp"
#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// Called once per verified record, in the order the records appear in the file.
//
//   offset -- where the record starts, which is what an index entry stores.
//   rec    -- the decoded record.
//   raw    -- the record's bytes exactly as they are on disk, crc included.
//
// `raw` is handed over so compaction can *copy* a record into the merged file
// rather than re-encode it. Re-encoding would produce the same bytes today, but
// copying makes the guarantee unconditional: what lands in the merged file is
// the byte sequence that just passed its crc check, not a fresh rendering of it
// that happens to agree. Valid only for the duration of the call.
//
// Returning a failure aborts the scan and is reported to the scan's caller.
using RecordVisitor = std::function<Status(std::uint64_t offset,
                                           const record::Record& rec,
                                           std::span<const std::uint8_t> raw)>;

// Walks a data file front to back, handing every intact record to `visit`, and
// stops at the first one that does not verify.
//
// Returns the offset just past the last good record -- equivalently, the length
// of the file's valid prefix. A return value equal to the file's size means the
// whole file is intact; anything less means the rest is unusable, and it is the
// *caller's* job to decide what that means. It has to be, because the answer
// depends on which file this is: an unfinished record at the end of the file
// being appended to is an ordinary torn write, while the same bytes in a sealed
// file are damage (see Bitcask::replay_file).
//
// The two reads per record are deliberate. Reading the 21-byte header first
// yields the declared total size, which is then bounded against the real file
// length *before* the second read allocates anything -- otherwise a corrupt
// header claiming a 4GB key is an instruction to allocate 4GB.
[[nodiscard]] Result<std::uint64_t> scan_records(const LogFile& file, const RecordVisitor& visit);

}  // namespace kvstore
