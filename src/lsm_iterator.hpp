#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/memtable.hpp"
#include "kvstore/result.hpp"
#include "kvstore/sstable.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// One ordered stream of entries, tombstones included.
//
// Tombstones are *in* the stream rather than filtered out by each source,
// because whether a tombstone matters depends on what the other sources say. A
// source that hid its own deletes would let an older source's value win, which
// is precisely the resurrection bug the tombstone exists to prevent. Filtering
// happens once, at the merge, where the whole picture is available.
class EntrySource {
public:
    virtual ~EntrySource() = default;

    [[nodiscard]] virtual bool valid() const = 0;
    [[nodiscard]] virtual std::string_view key() const = 0;
    [[nodiscard]] virtual std::string_view value() const = 0;
    [[nodiscard]] virtual bool tombstone() const = 0;
    [[nodiscard]] virtual Status advance() = 0;
};

// A range of a memtable.
//
// Holds a shared_ptr to it rather than a reference, because once the engine is
// concurrent an iterator can easily outlive the memtable it was opened against:
// a flush moves the live memtable aside and a background thread eventually drops
// it. Owning a handle is what makes the snapshot a snapshot.
class MemtableSource final : public EntrySource {
public:
    MemtableSource(std::shared_ptr<const Memtable> table, std::string_view begin,
                   std::string_view end);

    [[nodiscard]] bool valid() const override;
    [[nodiscard]] std::string_view key() const override;
    [[nodiscard]] std::string_view value() const override;
    [[nodiscard]] bool tombstone() const override;
    [[nodiscard]] Status advance() override;

private:
    std::shared_ptr<const Memtable> table_;  // Kept alive for the iterators below.
    Memtable::Map::const_iterator it_;
    Memtable::Map::const_iterator end_it_;
    std::string upper_;  // Exclusive. Empty means unbounded.
};

// A range of one SSTable, walked block by block.
//
// One block is held decoded at a time. The BlockEntry views point into block_,
// so every reassignment of block_ must be followed by a re-parse -- the two
// fields are one object with two halves, and load_block() is the only thing
// permitted to change either.
class TableSource final : public EntrySource {
public:
    // Takes a handle, not a reference: a compaction or a version swap can retire
    // this table while the iterator is still walking it, and the table (with its
    // mapping, which block_ may be a view into) has to outlive the walk.
    [[nodiscard]] static Result<std::unique_ptr<TableSource>> create(
        std::shared_ptr<const SSTable> table, std::string_view begin, std::string_view end);

    [[nodiscard]] bool valid() const override;
    [[nodiscard]] std::string_view key() const override;
    [[nodiscard]] std::string_view value() const override;
    [[nodiscard]] bool tombstone() const override;
    [[nodiscard]] Status advance() override;

private:
    explicit TableSource(std::shared_ptr<const SSTable> table) : table_(std::move(table)) {}

    [[nodiscard]] Status load_block(std::size_t index);
    void apply_upper_bound();

    std::shared_ptr<const SSTable> table_;
    std::size_t block_index_ = 0;
    sstable::BlockData block_;
    std::vector<sstable::BlockEntry> entries_;
    std::size_t entry_index_ = 0;
    std::string upper_;
    bool exhausted_ = true;
};

// A k-way merge over sources ordered newest first.
//
// Two rules, and they are the whole of LSM read semantics:
//
//   1. Of all sources positioned at the smallest key, the *newest* wins. Sources
//      arrive newest-first, so that is simply the lowest index.
//   2. If the winner is a tombstone, the key is skipped entirely -- and every
//      other source sitting on that key is advanced past it, which is what stops
//      an older table's value from surfacing.
//
// A linear scan over the sources per step rather than a heap. With a memtable,
// a flushing memtable and a handful of tables, k is small enough that a heap's
// bookkeeping costs more than it saves; when leveled compaction starts merging
// ten inputs at a time this is still true, and the note is here so the choice is
// a choice rather than an oversight.

// What the merge does with a key whose newest version is a tombstone.
//
// A reader wants Skip: a deleted key is not a row with no value, it is not a
// row. A *compaction* wants Emit, because a tombstone it is not yet allowed to
// drop has to be carried into the output file -- dropping it early would let the
// value in a lower level surface again. Same merge, two callers, and the
// difference between them is exactly one boolean.
enum class Tombstones { Skip, Emit };

class MergingIterator {
public:
    explicit MergingIterator(std::vector<std::unique_ptr<EntrySource>> sources,
                             Tombstones mode = Tombstones::Skip)
        : sources_(std::move(sources)), mode_(mode) {}

    // Positions on the first live key. Must be called once before valid().
    [[nodiscard]] Status seek_to_first() { return find_next_live(); }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    [[nodiscard]] std::string_view value() const noexcept { return value_; }

    // Only ever true under Tombstones::Emit; the value is empty when it is.
    [[nodiscard]] bool tombstone() const noexcept { return tombstone_; }

    [[nodiscard]] Status next() { return find_next_live(); }
    [[nodiscard]] Status status() const { return status_; }

private:
    [[nodiscard]] Status find_next_live();

    std::vector<std::unique_ptr<EntrySource>> sources_;  // Newest first.
    Tombstones mode_ = Tombstones::Skip;
    std::string key_;
    std::string value_;
    bool tombstone_ = false;
    bool valid_ = false;
    Status status_;
};

}  // namespace kvstore
