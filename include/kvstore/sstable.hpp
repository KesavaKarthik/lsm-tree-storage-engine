#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kvstore/bloom.hpp"
#include "kvstore/file_names.hpp"
#include "kvstore/log_file.hpp"
#include "kvstore/lsm_stats.hpp"
#include "kvstore/record.hpp"
#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// Defined in src/mapped_file.hpp. Incomplete here so that the platform headers
// it needs stay out of every consumer of this one.
class MappedFile;

// The SSTable: a sorted, immutable, block-structured file.
//
//   [ data block 0 ][ data block 1 ] ... [ data block N-1 ]
//   [ filter block ]        -- a bloom filter over every key in the table
//   [ index block ]
//   [ footer, 44 bytes, fixed size, at the very end ]
//
// Immutability is the load-bearing property, not a detail. A table is written
// once under a .tmp name, fsynced, and renamed into place complete; after that
// no byte of it ever changes. That is what lets a reader hold a pointer into it
// without a lock (Step 3), what makes memory-mapping it safe (Step 3, and the
// reason mmap was deferred until there was an immutable file to point it at),
// and what makes compaction a matter of writing new files and swapping which
// ones are current rather than editing anything.
//
// Sortedness is the other half. Because keys ascend through the file, a few
// hundred bytes of index can name the one block that could hold a given key, and
// two tables can be merged in a single sequential pass -- neither of which is
// available for Bitcask's write-ordered log.
namespace sstable {

// --- Data blocks -----------------------------------------------------------
//
//   block on disk = [ entry ][ entry ] ... [ crc32 4B ]
//   entry         = [ key_size 4B | value_size 4B | flags 1B | key | value ]
//
// The crc is per *block*, not per record and not per file. Per record would
// repeat the Bitcask overhead for no gain here, since a block is the smallest
// unit ever read. Per file would mean verifying a gigabyte to answer one get().
// The block is exactly the granularity at which bytes are fetched, so it is the
// granularity at which they are checked -- which keeps the Phase 1 promise that
// nothing is returned to a caller without having been verified first.
//
// flags reuses record::kFlagTombstone, the same bit in the same position as the
// Bitcask record header. Two formats in one engine disagreeing about which bit
// means "deleted" is a bug waiting for a quiet afternoon.
inline constexpr std::size_t kEntryHeaderSize = 9;  // 4 + 4 + 1
inline constexpr std::size_t kBlockTrailerSize = 4;
inline constexpr std::uint8_t kFlagTombstone = record::kFlagTombstone;

// Bytes a block reaches before the builder starts a new one. Blocks are cut at
// an entry boundary, never inside one, so an oversized entry gets a block of its
// own -- the same rule rotate_if_needed() uses for an oversized record.
inline constexpr std::uint32_t kDefaultBlockSize = 4096;

// One decoded entry, pointing *into* the caller's block buffer rather than
// owning copies. A get() that copied every key and value in a block just to
// throw all but one away would be paying for the block twice.
struct BlockEntry {
    std::string_view key;
    std::string_view value;
    bool tombstone = false;
};

// Decodes a block payload (the crc trailer already stripped and verified).
// The returned views are valid only while `payload` is.
[[nodiscard]] Status parse_block(std::span<const std::uint8_t> payload,
                                 std::vector<BlockEntry>* out);

// A block's bytes, however they were obtained.
//
// Under ReadMode::Pread the block was copied out of the kernel into a buffer
// this owns. Under ReadMode::Mmap it is a *view* straight into the mapping, and
// nothing was copied at all -- which is the entire point of mapping the file, and
// the reason read_block() cannot simply return a vector.
//
// A borrowed view is valid for as long as the SSTable that produced it, and no
// longer. Once the engine is concurrent that is guaranteed structurally: a
// reader holds the Version, the Version holds the table, and the table holds the
// mapping.
class BlockData {
public:
    BlockData() = default;

    static BlockData owning(std::vector<std::uint8_t> bytes) {
        BlockData data;
        data.owned_ = std::move(bytes);
        data.owns_ = true;
        return data;
    }

    static BlockData borrowed(std::span<const std::uint8_t> bytes) {
        BlockData data;
        data.view_ = bytes;
        return data;
    }

    // Computed rather than stored, so moving this object cannot leave a span
    // pointing at a vector that has moved out from under it.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return owns_ ? std::span<const std::uint8_t>{owned_.data(), owned_.size()} : view_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes().size(); }
    [[nodiscard]] bool owns() const noexcept { return owns_; }

    // Drops everything past `size` -- used to strip a block's crc trailer once
    // it has been checked. Free for a borrowed view, and a shrink that never
    // reallocates for an owned buffer, so neither path pays for a second copy.
    void trim(std::size_t size) noexcept {
        if (owns_) {
            if (size < owned_.size()) {
                owned_.resize(size);
            }
        } else if (size < view_.size()) {
            view_ = view_.subspan(0, size);
        }
    }

private:
    std::vector<std::uint8_t> owned_;
    std::span<const std::uint8_t> view_;
    bool owns_ = false;
};

// --- Index block -----------------------------------------------------------
//
//   index block = [ entry_count 4B ][ index entry ] ... [ crc32 4B ]
//   index entry = [ key_size 4B | key | block_offset 8B | block_size 4B ]
//
// One entry per block, which *is* the sparse index the design called for: it
// keeps one key per block resident instead of one key per key, with the sparsity
// N self-tuning to the key size instead of being a constant somebody has to
// guess. This is the concrete answer to Bitcask's "every key must fit in RAM" --
// a 64MiB table with 100-byte keys needs a few hundred KiB of index, not the
// whole keyspace.
//
// **The key stored is the LAST key in the block, not the first.** That makes a
// lookup a single binary search for the first entry whose key >= the target,
// which names exactly one candidate block: everything in earlier blocks is
// smaller, everything in later blocks is larger. Storing first keys would need
// the same search shifted by one and an awkward edge case at the front. It also
// makes max_key() free.
inline constexpr std::size_t kIndexHeaderSize = 4;
inline constexpr std::size_t kIndexEntryFixedSize = 16;  // key_size + offset + size

struct IndexEntry {
    std::string key;  // The last key in the block, inclusive.
    std::uint64_t block_offset = 0;
    std::uint32_t block_size = 0;  // On disk, crc trailer included.
};

// --- Footer ----------------------------------------------------------------
//
//   [ index_offset 8B | index_size 4B | filter_offset 8B | filter_size 4B
//   | entry_count 8B | format_version 4B | crc32 4B | magic "KVST" 4B ]
//
// Fixed size and last, so a reader finds it by seeking to end-minus-44 without
// knowing anything else about the file. Magic goes at the very end because that
// is where a reader looks first; the crc covers the 36 bytes before it.
//
// **filter_offset and filter_size point at the bloom filter, and the slot was
// reserved a phase before anything filled it.** That is why adding filters cost
// no format change: format_version is still 1, a table written before they
// existed still reads (it simply has filter_size == 0 and no filter to consult),
// and a table written now reads on a build that ignores the slot. Reserving
// twelve bytes bought forward and backward compatibility for free.
//
// format_version exists for the changes that *cannot* be made compatibly: it
// lets the file say so, instead of being silently misread -- which is the
// failure a magic number alone does not catch, because the magic is still right.
inline constexpr std::size_t kFooterSize = 44;
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint8_t kMagic[4] = {'K', 'V', 'S', 'T'};

struct Footer {
    std::uint64_t index_offset = 0;
    std::uint32_t index_size = 0;
    std::uint64_t filter_offset = 0;
    std::uint32_t filter_size = 0;
    std::uint64_t entry_count = 0;
    std::uint32_t format_version = kFormatVersion;
};

[[nodiscard]] std::vector<std::uint8_t> encode_footer(const Footer& footer);
[[nodiscard]] Result<Footer> decode_footer(std::span<const std::uint8_t> bytes);

}  // namespace sstable

// Writes one SSTable, in one forward pass, from keys handed over in sorted
// order.
//
// There is no sorting step and no buffering of the whole table, because the
// memtable that feeds it is already sorted -- which is the payoff for having
// paid for an ordered structure on the write path. A flush is therefore a
// sequential write of exactly the bytes the file will contain.
//
// Writes to a temp path. finish() fsyncs and closes but deliberately does *not*
// rename: installing the file is the caller's decision, because the caller is
// the one that knows what else has to be durable first.
class SSTableBuilder {
public:
    [[nodiscard]] static Result<SSTableBuilder> create(
        const std::filesystem::path& temp_path, std::uint32_t block_size,
        std::uint32_t bits_per_key = bloom::kDefaultBitsPerKey);

    SSTableBuilder(SSTableBuilder&&) noexcept = default;
    SSTableBuilder& operator=(SSTableBuilder&&) noexcept = default;
    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    // Keys must arrive in strictly increasing order. Violating that is
    // InvalidArgument rather than something the builder quietly fixes: it means
    // the caller's source was not sorted, and a table built from an unsorted
    // source would pass every write-side check and then fail lookups at random
    // months later.
    //
    // **Tombstones go into the bloom filter like any other key.** A tombstone is
    // an answer, not an absence: if the filter denied a deleted key, the lookup
    // would skip this table, find the old value in an older one, and the delete
    // would silently undo itself. It is one line in add(), and it is the
    // easiest thing in this phase to get wrong without any test noticing.
    [[nodiscard]] Status add(std::string_view key, std::string_view value, bool tombstone);

    // Flushes the last block, writes the filter, index and footer, fsyncs, and
    // closes the descriptor.
    [[nodiscard]] Status finish();

    [[nodiscard]] std::uint64_t entry_count() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t file_size() const noexcept { return file_.size(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return file_.path(); }

private:
    SSTableBuilder(LogFile file, std::uint32_t block_size, std::uint32_t bits_per_key)
        : file_(std::move(file)), block_size_(block_size), filter_(bits_per_key) {}

    [[nodiscard]] Status flush_block();

    LogFile file_;
    std::uint32_t block_size_ = sstable::kDefaultBlockSize;

    // Fed by every add(), tombstones included -- see the note on add().
    BloomBuilder filter_;

    std::vector<std::uint8_t> block_;  // The block being filled.
    std::string block_last_key_;       // Last key in `block_`; becomes its index key.
    std::string last_key_;             // Last key added at all; enforces the ordering.
    bool has_last_key_ = false;

    std::vector<sstable::IndexEntry> index_;
    std::uint64_t entries_ = 0;
    bool finished_ = false;
};

// What a table had to say about a key.
//
// Three outcomes, not two, and the third is the one that matters: a tombstone is
// not "absent". "Absent" means keep looking in older tables; "deleted" means
// stop, the answer is NotFound, and an older table's value must not be
// consulted. Collapsing these two into one bool is how a delete gets undone by a
// compaction six months later.
enum class Lookup {
    NotPresent,
    Found,
    Deleted,
};

// How a table's data blocks are fetched.
//
// **Pread is the default, and that is a deliberate choice about failure rather
// than about speed.** pread copies a kernel page into a buffer and hands back an
// errno that becomes a Status; a mapping hands over the kernel page itself, with
// no syscall and no copy -- and turns an I/O error into SIGBUS, which kills the
// process with nothing to report. This engine returns failures rather than
// throwing them, so the reportable path is the one you get by default and the
// faster one is asked for explicitly.
enum class ReadMode {
    Pread,
    Mmap,
};

// Reads one SSTable. Immutable, and safe to share across threads.
class SSTable {
public:
    // Opens the file and loads its index and filter into memory. The data blocks
    // stay on disk. Fails with Corruption if the footer, the index, or the
    // offsets in them do not describe a file of this size.
    //
    // `bloom_enabled` false loads the filter as usual but never consults it on
    // the read path. It exists to measure what the filter is worth: both arms of
    // the comparison then run against byte-identical files, which they would not
    // if the filter were omitted at write time instead. See LsmOptions.
    [[nodiscard]] static Result<std::unique_ptr<SSTable>> open(const std::filesystem::path& path,
                                                               FileId id,
                                                               LsmStats* stats = nullptr,
                                                               ReadMode mode = ReadMode::Pread,
                                                               bool bloom_enabled = true);

    // Out of line: MappedFile is incomplete here on purpose, so <sys/mman.h> and
    // <windows.h> stay out of every translation unit that includes this header.
    ~SSTable();

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;

    // Fills `value` only on Lookup::Found.
    [[nodiscard]] Result<Lookup> lookup(std::string_view key, std::string* value) const;

    // One block's payload, crc verified. Exposed for the merging iterator, which
    // walks blocks in order rather than searching for one.
    //
    // Under ReadMode::Mmap the result borrows from the mapping and is valid only
    // while this table is alive; see BlockData.
    [[nodiscard]] Result<sstable::BlockData> read_block(std::size_t block_index) const;

    [[nodiscard]] FileId id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t entry_count() const noexcept { return footer_.entry_count; }
    [[nodiscard]] std::uint64_t file_size() const noexcept { return file_size_; }
    [[nodiscard]] std::size_t block_count() const noexcept { return index_.size(); }
    [[nodiscard]] const std::vector<sstable::IndexEntry>& index() const noexcept { return index_; }

    // Empty for an empty table.
    [[nodiscard]] const std::string& min_key() const noexcept { return min_key_; }
    [[nodiscard]] const std::string& max_key() const noexcept { return max_key_; }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Releases the mapping and the descriptor before the destructor would, so
    // the file can be renamed over or unlinked on Windows -- which refuses both
    // while either is live. Mapping first: a view keeps the file open behind it.
    [[nodiscard]] Status close();

    // Marks the file for deletion when the last reference to this table goes
    // away. Compaction calls it after the manifest that stops naming this table
    // has been committed -- from that moment the file is unreachable, but a
    // reader that started before the swap may still be inside it.
    //
    // mutable + atomic so it works through the shared_ptr<const SSTable> a
    // Version holds: the *file* is being retired, not the table's contents.
    void mark_obsolete() const noexcept { obsolete_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool is_obsolete() const noexcept {
        return obsolete_.load(std::memory_order_relaxed);
    }

private:
    // Out of line for the same reason the destructor is: a unique_ptr member to
    // an incomplete type needs that type complete wherever the compiler might
    // have to destroy it, and a constructor is one of those places.
    SSTable(LogFile file, std::filesystem::path path, FileId id, LsmStats* stats, ReadMode mode,
            bool bloom_enabled);

    [[nodiscard]] Status load();

    // Decodes the index block's payload (crc already stripped and verified).
    [[nodiscard]] Status load_index(std::span<const std::uint8_t> payload);

    // Reads and decodes the filter block, if this table has one.
    [[nodiscard]] Status load_filter();

    // read_block() without the statistics increment. Used by load(), because a
    // block read that happens at open time is not read amplification for any
    // get() and counting it would make the Step 2 assertion "an absent key costs
    // no block reads" quietly untrue.
    [[nodiscard]] Result<sstable::BlockData> read_block_raw(std::size_t block_index) const;

    // `size` bytes at `offset`, through whichever path this table was opened
    // with. The one place the two read modes differ.
    [[nodiscard]] Result<sstable::BlockData> read_at(std::uint64_t offset,
                                                     std::uint32_t size) const;

    // Maps the file and releases the descriptor. Only under ReadMode::Mmap.
    [[nodiscard]] Status map_file();

    LogFile file_;

    // Kept separately from file_, because in mmap mode the descriptor is closed
    // once the mapping exists and file_ no longer knows its own name.
    std::filesystem::path path_;

    // Present only under ReadMode::Mmap. unique_ptr to an incomplete type, so
    // the platform headers stay in src/.
    std::unique_ptr<MappedFile> mapping_;

    FileId id_ = 0;
    LsmStats* stats_ = nullptr;  // Not owned; may be null.
    ReadMode read_mode_ = ReadMode::Pread;

    // False makes lookup() skip the filter probe. Measurement only; see open().
    bool bloom_enabled_ = true;

    mutable std::atomic<bool> obsolete_{false};

    sstable::Footer footer_;
    std::vector<sstable::IndexEntry> index_;

    // Absent for a table written before filters existed, which is a slower
    // lookup rather than a wrong one.
    std::optional<BloomFilter> filter_;

    std::uint64_t file_size_ = 0;
    std::string min_key_;
    std::string max_key_;
};

}  // namespace kvstore
