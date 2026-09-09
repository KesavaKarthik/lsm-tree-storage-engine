#include "kvstore/sstable.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "encoding.hpp"
#include "mapped_file.hpp"

namespace kvstore {

namespace {

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    encoding::put_u32(out, at, v);
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    encoding::put_u64(out, at, v);
}

void append_bytes(std::vector<std::uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
}

// A view into a byte span. Length zero yields an empty view rather than a
// pointer one past the end, which an empty value would otherwise produce.
std::string_view as_view(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t len) {
    if (len == 0) {
        return std::string_view{};
    }
    return std::string_view{reinterpret_cast<const char*>(bytes.data() + at), len};
}

}  // namespace

namespace sstable {

Status parse_block(std::span<const std::uint8_t> payload, std::vector<BlockEntry>* out) {
    if (out == nullptr) {
        return Status::invalid_argument("entry out-parameter must not be null");
    }
    out->clear();

    std::size_t at = 0;
    while (at < payload.size()) {
        if (payload.size() - at < kEntryHeaderSize) {
            return Status::corruption("sstable block ends inside an entry header");
        }
        const std::uint32_t key_size = encoding::get_u32(payload, at);
        const std::uint32_t value_size = encoding::get_u32(payload, at + 4);
        const std::uint8_t flags = payload[at + 8];
        at += kEntryHeaderSize;

        // The sizes came off disk. The block's crc says the bytes are the ones
        // we wrote, but a table written by a different version -- or simply a
        // bug -- could still have written sizes that do not fit, and an
        // unchecked size here is a read past the end of the buffer.
        const std::uint64_t need = static_cast<std::uint64_t>(key_size) + value_size;
        if (need > payload.size() - at) {
            return Status::corruption("sstable block entry claims more bytes than the block holds");
        }
        if (key_size == 0) {
            return Status::corruption("sstable block entry has an empty key");
        }

        BlockEntry entry;
        entry.key = as_view(payload, at, key_size);
        entry.value = as_view(payload, at + key_size, value_size);
        entry.tombstone = (flags & kFlagTombstone) != 0;
        out->push_back(entry);

        at += static_cast<std::size_t>(need);
    }
    return {};
}

std::vector<std::uint8_t> encode_footer(const Footer& footer) {
    std::vector<std::uint8_t> out;
    out.reserve(kFooterSize);

    append_u64(out, footer.index_offset);
    append_u32(out, footer.index_size);
    append_u64(out, footer.filter_offset);
    append_u32(out, footer.filter_size);
    append_u64(out, footer.entry_count);
    append_u32(out, footer.format_version);

    const std::uint32_t crc = record::crc32_of(out);
    append_u32(out, crc);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));

    return out;
}

Result<Footer> decode_footer(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kFooterSize) {
        return Status::corruption("sstable footer is not the expected size");
    }
    // Magic first, so "this is not one of our files at all" reads differently
    // from "this is one of our files and it is damaged".
    if (std::memcmp(bytes.data() + 40, kMagic, sizeof(kMagic)) != 0) {
        return Status::corruption("not an sstable: bad footer magic");
    }
    if (encoding::get_u32(bytes, 36) != record::crc32_of(bytes.subspan(0, 36))) {
        return Status::corruption("sstable footer fails its crc");
    }

    Footer footer;
    footer.index_offset = encoding::get_u64(bytes, 0);
    footer.index_size = encoding::get_u32(bytes, 8);
    footer.filter_offset = encoding::get_u64(bytes, 12);
    footer.filter_size = encoding::get_u32(bytes, 20);
    footer.entry_count = encoding::get_u64(bytes, 24);
    footer.format_version = encoding::get_u32(bytes, 32);

    if (footer.format_version != kFormatVersion) {
        return Status::corruption("unsupported sstable format version " +
                                  std::to_string(footer.format_version));
    }
    return footer;
}

}  // namespace sstable

// --- SSTableBuilder --------------------------------------------------------

Result<SSTableBuilder> SSTableBuilder::create(const std::filesystem::path& temp_path,
                                              std::uint32_t block_size,
                                              std::uint32_t bits_per_key) {
    if (block_size == 0) {
        return Status::invalid_argument("block size must not be zero");
    }

    // Remove any stale temp first. LogFile::open() positions at the end of an
    // existing file, so a leftover from an interrupted flush would be *appended
    // to* rather than replaced -- producing a file with two footers, the first
    // of which is garbage in the middle of the data region.
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);

    // Never: the builder fsyncs once, in finish(). An fsync per block would be a
    // device round trip per 4KiB of a file that is worth nothing at all until it
    // is complete.
    auto file = LogFile::open(temp_path, SyncMode::Never);
    if (!file.is_ok()) {
        return file.status();
    }
    return SSTableBuilder{file.take(), block_size, bits_per_key};
}

Status SSTableBuilder::add(std::string_view key, std::string_view value, bool tombstone) {
    if (finished_) {
        return Status::invalid_argument("add() after finish()");
    }
    if (key.empty()) {
        return Status::invalid_argument("key must not be empty");
    }
    if (has_last_key_ && key <= std::string_view{last_key_}) {
        return Status::invalid_argument("sstable keys must be added in strictly increasing order");
    }

    const std::string_view stored = tombstone ? std::string_view{} : value;
    const std::uint64_t entry_size = sstable::kEntryHeaderSize + key.size() + stored.size();
    if (entry_size > record::kMaxRecordSize) {
        return Status::invalid_argument("entry exceeds the maximum record size");
    }

    // Every key, tombstones included. See the note in the header: a filter that
    // denied a deleted key would let an older table's value surface.
    filter_.add(key);

    append_u32(block_, static_cast<std::uint32_t>(key.size()));
    append_u32(block_, static_cast<std::uint32_t>(stored.size()));
    block_.push_back(tombstone ? sstable::kFlagTombstone : std::uint8_t{0});
    append_bytes(block_, key);
    append_bytes(block_, stored);

    block_last_key_.assign(key);
    last_key_.assign(key);
    has_last_key_ = true;
    ++entries_;

    // Cut at an entry boundary once the block has reached its target. An entry
    // is never split across blocks, so an entry larger than the target simply
    // gets a block to itself -- the same rule Bitcask uses for a record larger
    // than a data file.
    if (block_.size() >= block_size_) {
        KVSTORE_RETURN_IF_ERROR(flush_block());
    }
    return {};
}

Status SSTableBuilder::flush_block() {
    if (block_.empty()) {
        return {};
    }
    if (block_.size() + sstable::kBlockTrailerSize > std::numeric_limits<std::uint32_t>::max()) {
        return Status::invalid_argument("sstable block exceeds the maximum block size");
    }

    const std::uint32_t crc = record::crc32_of(block_);
    append_u32(block_, crc);

    auto offset = file_.append(block_);
    if (!offset.is_ok()) {
        return offset.status();
    }

    sstable::IndexEntry entry;
    entry.key = block_last_key_;
    entry.block_offset = *offset;
    entry.block_size = static_cast<std::uint32_t>(block_.size());
    index_.push_back(std::move(entry));

    block_.clear();
    block_last_key_.clear();
    return {};
}

Status SSTableBuilder::finish() {
    if (finished_) {
        return Status::invalid_argument("finish() called twice");
    }
    KVSTORE_RETURN_IF_ERROR(flush_block());

    sstable::Footer footer;
    footer.entry_count = entries_;

    // The filter block goes between the data and the index, in the slot the
    // footer reserved before there was anything to put in it. Its offset also
    // gives the data region a definite end -- everything before filter_offset is
    // data blocks, which is what the index bound check in load() relies on.
    const std::vector<std::uint8_t> filter_bytes = filter_.finish();
    if (filter_bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::invalid_argument("sstable filter exceeds the maximum filter size");
    }
    auto filter_offset = file_.append(filter_bytes);
    if (!filter_offset.is_ok()) {
        return filter_offset.status();
    }
    footer.filter_offset = *filter_offset;
    footer.filter_size = static_cast<std::uint32_t>(filter_bytes.size());

    std::vector<std::uint8_t> index_bytes;
    append_u32(index_bytes, static_cast<std::uint32_t>(index_.size()));
    for (const sstable::IndexEntry& entry : index_) {
        append_u32(index_bytes, static_cast<std::uint32_t>(entry.key.size()));
        append_bytes(index_bytes, entry.key);
        append_u64(index_bytes, entry.block_offset);
        append_u32(index_bytes, entry.block_size);
    }
    const std::uint32_t index_crc = record::crc32_of(index_bytes);
    append_u32(index_bytes, index_crc);

    if (index_bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::invalid_argument("sstable index exceeds the maximum index size");
    }

    auto index_offset = file_.append(index_bytes);
    if (!index_offset.is_ok()) {
        return index_offset.status();
    }
    footer.index_offset = *index_offset;
    footer.index_size = static_cast<std::uint32_t>(index_bytes.size());

    const std::vector<std::uint8_t> footer_bytes = sstable::encode_footer(footer);
    auto footer_offset = file_.append(footer_bytes);
    if (!footer_offset.is_ok()) {
        return footer_offset.status();
    }

    // fsync here, before the caller renames. The rename is the commit point, and
    // committing a name for bytes that are still only in the page cache commits
    // nothing -- a power cut then leaves a file that exists, has the right name,
    // and is full of zeroes.
    KVSTORE_RETURN_IF_ERROR(file_.sync());

    // Close before returning, because the caller's next move is a rename over
    // this name, and Windows will not rename a file this process holds open.
    KVSTORE_RETURN_IF_ERROR(file_.close());

    finished_ = true;
    return {};
}

// --- SSTable ---------------------------------------------------------------

Result<std::unique_ptr<SSTable>> SSTable::open(const std::filesystem::path& path, FileId id,
                                               LsmStats* stats, ReadMode mode,
                                               bool bloom_enabled) {
    // Checked explicitly, because LogFile::open() *creates* what it cannot find.
    // Without this, opening a table that isn't there would silently manufacture
    // an empty file and then report it as corrupt -- and leave it behind.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Status::io_error("no such sstable: " + path.string());
    }

    auto file = LogFile::open(path, SyncMode::Never);
    if (!file.is_ok()) {
        return file.status();
    }

    // `new` rather than make_unique: the constructor is private, and befriending
    // make_unique would let anyone construct one through it.
    std::unique_ptr<SSTable> table{
        new SSTable{file.take(), path, id, stats, mode, bloom_enabled}};
    KVSTORE_RETURN_IF_ERROR(table->load());
    return table;
}

SSTable::SSTable(LogFile file, std::filesystem::path path, FileId id, LsmStats* stats,
                 ReadMode mode, bool bloom_enabled)
    : file_(std::move(file)),
      path_(std::move(path)),
      id_(id),
      stats_(stats),
      read_mode_(mode),
      bloom_enabled_(bloom_enabled) {}

SSTable::~SSTable() {
    // Order matters and is the whole reason this is written out rather than
    // defaulted. On Windows a file cannot be unlinked while a mapping or a
    // handle is live, so both go first -- and the unlink has to happen here,
    // because this is the moment the last reader finished with the table.
    if (mapping_ != nullptr) {
        (void)mapping_->close();
        mapping_.reset();
    }
    (void)file_.close();

    if (is_obsolete()) {
        // Best effort, and it is allowed to be. A destructor has nothing to
        // report a failure to -- but a file that survives is one the manifest
        // does not name, and recovery's orphan rule sweeps it at the next open.
        // Step 2's commit rule is what makes this safe to shrug at.
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

Status SSTable::close() {
    Status result;
    if (mapping_ != nullptr) {
        result = mapping_->close();
        mapping_.reset();
    }
    // The descriptor closes either way; a failed unmap must not leave it open.
    const Status closed = file_.close();
    return result.is_ok() ? closed : result;
}

Status SSTable::load() {
    file_size_ = file_.size();
    if (file_size_ < sstable::kFooterSize) {
        return Status::corruption("sstable is too small to hold a footer: " + path_.string());
    }

    // The mapping goes up before anything is read, so the footer, the index and
    // the filter all arrive through the same path the data blocks will.
    if (read_mode_ == ReadMode::Mmap) {
        KVSTORE_RETURN_IF_ERROR(map_file());
    }

    auto footer_bytes = read_at(file_size_ - sstable::kFooterSize,
                                static_cast<std::uint32_t>(sstable::kFooterSize));
    if (!footer_bytes.is_ok()) {
        return footer_bytes.status();
    }
    auto footer = sstable::decode_footer(footer_bytes->bytes());
    if (!footer.is_ok()) {
        return footer.status();
    }
    footer_ = *footer;

    // Every offset below came off disk. The footer's crc says these are the
    // bytes we wrote, which is not the same as saying they describe this file --
    // so each region is bounded against the real length before it is read.
    // Subtraction rather than addition throughout: `a + b > limit` overflows
    // where `a > limit - b` cannot.
    const std::uint64_t metadata_start = file_size_ - sstable::kFooterSize;
    if (footer_.index_offset > metadata_start ||
        footer_.index_size > metadata_start - footer_.index_offset) {
        return Status::corruption("sstable index block runs past the end of the file");
    }
    if (footer_.filter_offset > metadata_start ||
        footer_.filter_size > metadata_start - footer_.filter_offset) {
        return Status::corruption("sstable filter block runs past the end of the file");
    }
    if (footer_.index_size < sstable::kIndexHeaderSize + sstable::kBlockTrailerSize) {
        return Status::corruption("sstable index block is too small to be one");
    }

    auto index_bytes = read_at(footer_.index_offset, footer_.index_size);
    if (!index_bytes.is_ok()) {
        return index_bytes.status();
    }
    const std::span<const std::uint8_t> raw = index_bytes->bytes();
    const std::size_t payload_size = raw.size() - sstable::kBlockTrailerSize;
    if (encoding::get_u32(raw, payload_size) != record::crc32_of(raw.subspan(0, payload_size))) {
        return Status::corruption("sstable index block fails its crc: " + path_.string());
    }

    KVSTORE_RETURN_IF_ERROR(load_index(raw.subspan(0, payload_size)));
    KVSTORE_RETURN_IF_ERROR(load_filter());

    if (index_.empty()) {
        if (footer_.entry_count != 0) {
            return Status::corruption("sstable declares entries but its index names no blocks");
        }
        return {};
    }

    // max_key is free: the index stores each block's last key, so the last
    // index entry holds the largest key in the table.
    max_key_ = index_.back().key;

    // min_key costs one block read, paid once at open. The alternative -- a
    // min_key field in the footer -- would make the footer variable-length, and
    // a fixed-size footer is precisely what lets a reader find it by seeking to
    // end-minus-44 while knowing nothing else. Reading block 0 here also proves
    // the data region is readable at open time rather than on the first get().
    auto block = read_block_raw(0);
    if (!block.is_ok()) {
        return block.status();
    }
    std::vector<sstable::BlockEntry> entries;
    KVSTORE_RETURN_IF_ERROR(sstable::parse_block(block->bytes(), &entries));
    if (entries.empty()) {
        return Status::corruption("sstable first data block is empty");
    }
    min_key_.assign(entries.front().key);
    return {};
}

Status SSTable::map_file() {
    auto mapped = MappedFile::open(path_);
    if (!mapped.is_ok()) {
        return mapped.status();
    }
    mapping_ = std::make_unique<MappedFile>(mapped.take());

    // Point lookups defeat readahead by construction: the next block wanted is
    // almost never the next block on disk. Best effort -- a hint the kernel
    // ignores costs throughput, never correctness.
    (void)mapping_->advise_random();

    // The descriptor has nothing left to do. The mapping holds its own reference
    // to the file, so a mapped table costs an address range and no fd at all.
    return file_.close();
}

Status SSTable::load_filter() {
    // A table written before filters existed has nothing here. That is a slower
    // lookup, not a wrong one -- and it is the reason adding filters needed no
    // format version bump.
    if (footer_.filter_size == 0) {
        return {};
    }

    auto bytes = read_at(footer_.filter_offset, footer_.filter_size);
    if (!bytes.is_ok()) {
        return bytes.status();
    }
    auto filter = BloomFilter::decode(bytes->bytes());
    if (!filter.is_ok()) {
        return filter.status();
    }
    filter_ = filter.take();
    return {};
}

Status SSTable::load_index(std::span<const std::uint8_t> payload) {
    const std::uint32_t count = encoding::get_u32(payload, 0);

    // Bound the declared count against what the block could physically hold
    // before reserving for it. A corrupt count is otherwise an instruction to
    // allocate four billion index entries.
    if (static_cast<std::uint64_t>(count) * sstable::kIndexEntryFixedSize >
        payload.size() - sstable::kIndexHeaderSize) {
        return Status::corruption("sstable index declares more entries than it could hold");
    }
    index_.reserve(count);

    std::size_t at = sstable::kIndexHeaderSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (payload.size() - at < sstable::kIndexEntryFixedSize) {
            return Status::corruption("sstable index ends inside an entry");
        }
        const std::uint32_t key_size = encoding::get_u32(payload, at);
        at += 4;
        if (key_size == 0 || key_size > payload.size() - at ||
            payload.size() - at - key_size < 12) {
            return Status::corruption("sstable index entry claims more bytes than it holds");
        }

        sstable::IndexEntry entry;
        entry.key.assign(as_view(payload, at, key_size));
        at += key_size;
        entry.block_offset = encoding::get_u64(payload, at);
        at += 8;
        entry.block_size = encoding::get_u32(payload, at);
        at += 4;

        // A block must lie wholly inside the data region -- which ends where the
        // filter block begins -- and be large enough to carry a crc trailer.
        if (entry.block_size < sstable::kBlockTrailerSize ||
            entry.block_offset > footer_.filter_offset ||
            entry.block_size > footer_.filter_offset - entry.block_offset) {
            return Status::corruption("sstable index points outside the data region");
        }
        // The binary search in lookup() is only meaningful if this holds, so it
        // is checked once at open rather than assumed on every read.
        if (!index_.empty() && entry.key <= index_.back().key) {
            return Status::corruption("sstable index keys are not strictly increasing");
        }
        index_.push_back(std::move(entry));
    }

    if (at != payload.size()) {
        return Status::corruption("sstable index has trailing bytes after its last entry");
    }
    return {};
}

// The one place the two read modes differ.
Result<sstable::BlockData> SSTable::read_at(std::uint64_t offset, std::uint32_t size) const {
    if (mapping_ != nullptr) {
        const std::span<const std::uint8_t> all = mapping_->bytes();

        // **The bound check LogFile was doing for us.** A pread that runs past
        // the end returns a short count, which LogFile turns into Corruption. A
        // mapping has no such courtesy: an out-of-range offset is simply a wild
        // pointer into whatever follows the mapping, or a fault. So the check
        // that came free on one path has to be written out on the other.
        if (offset > all.size() || size > all.size() - offset) {
            return Status::corruption("sstable read runs past the end of the mapping: " +
                                      path_.string());
        }
        return sstable::BlockData::borrowed(all.subspan(static_cast<std::size_t>(offset), size));
    }

    auto bytes = file_.read_at(offset, size);
    if (!bytes.is_ok()) {
        return bytes.status();
    }
    return sstable::BlockData::owning(bytes.take());
}

Result<sstable::BlockData> SSTable::read_block(std::size_t block_index) const {
    if (stats_ != nullptr) {
        LsmStats::bump(stats_->block_reads);
    }
    return read_block_raw(block_index);
}

Result<sstable::BlockData> SSTable::read_block_raw(std::size_t block_index) const {
    if (block_index >= index_.size()) {
        return Status::invalid_argument("sstable block index out of range");
    }
    const sstable::IndexEntry& entry = index_[block_index];

    auto block = read_at(entry.block_offset, entry.block_size);
    if (!block.is_ok()) {
        return block.status();
    }

    // The crc is verified identically on both paths -- the mapped path is not a
    // faster way to skip checking, only a faster way to get the bytes.
    const std::span<const std::uint8_t> raw = block->bytes();
    const std::size_t payload_size = raw.size() - sstable::kBlockTrailerSize;
    if (encoding::get_u32(raw, payload_size) !=
        record::crc32_of(raw.subspan(0, payload_size))) {
        return Status::corruption("sstable data block fails its crc: " + path_.string());
    }

    // Hand back the payload; the trailer has done its job.
    block->trim(payload_size);
    return block.take();
}

Result<Lookup> SSTable::lookup(std::string_view key, std::string* value) const {
    if (value == nullptr) {
        return Status::invalid_argument("value out-parameter must not be null");
    }
    if (index_.empty()) {
        return Lookup::NotPresent;
    }

    // Two comparisons that cost nothing and save a whole block read each: they
    // reject every key outside the table's range before anything touches a disk.
    if (key < std::string_view{min_key_} || key > std::string_view{max_key_}) {
        return Lookup::NotPresent;
    }

    // And then the filter, which rejects most of the keys that fall *inside* the
    // range and still are not here. A false answer is final and costs nothing
    // but RAM; a true answer is a "maybe" and the search continues below. This
    // one branch is what turns "an absent key must interrogate every table" into
    // "an absent key touches no disk at all".
    if (bloom_enabled_ && filter_.has_value()) {
        if (stats_ != nullptr) {
            LsmStats::bump(stats_->bloom_checks);
        }
        if (!filter_->maybe_contains(key)) {
            if (stats_ != nullptr) {
                LsmStats::bump(stats_->bloom_rejects);
            }
            return Lookup::NotPresent;
        }
    }

    // The index holds each block's *last* key, so the first entry whose key is
    // >= the target names the only block that could contain it.
    const auto it = std::lower_bound(index_.begin(), index_.end(), key,
                                     [](const sstable::IndexEntry& entry, std::string_view target) {
                                         return std::string_view{entry.key} < target;
                                     });
    if (it == index_.end()) {
        return Lookup::NotPresent;
    }

    auto block = read_block(static_cast<std::size_t>(it - index_.begin()));
    if (!block.is_ok()) {
        return block.status();
    }
    std::vector<sstable::BlockEntry> entries;
    KVSTORE_RETURN_IF_ERROR(sstable::parse_block(block->bytes(), &entries));

    const auto hit = std::lower_bound(entries.begin(), entries.end(), key,
                                      [](const sstable::BlockEntry& entry, std::string_view target) {
                                          return entry.key < target;
                                      });
    if (hit == entries.end() || hit->key != key) {
        return Lookup::NotPresent;
    }
    if (hit->tombstone) {
        return Lookup::Deleted;
    }

    value->assign(hit->value);
    return Lookup::Found;
}

}  // namespace kvstore
