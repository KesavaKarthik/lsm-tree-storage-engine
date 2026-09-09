#include "lsm_iterator.hpp"

#include <algorithm>
#include <utility>

#include "kvstore/lsm.hpp"

namespace kvstore {

// --- MemtableSource --------------------------------------------------------

MemtableSource::MemtableSource(std::shared_ptr<const Memtable> table, std::string_view begin,
                               std::string_view end)
    : table_(std::move(table)),
      it_(begin.empty() ? table_->begin() : table_->lower_bound(begin)),
      end_it_(table_->end()),
      upper_(end) {}

bool MemtableSource::valid() const {
    if (it_ == end_it_) {
        return false;
    }
    return upper_.empty() || it_->first < upper_;
}

std::string_view MemtableSource::key() const { return it_->first; }

std::string_view MemtableSource::value() const { return it_->second.value; }

bool MemtableSource::tombstone() const { return it_->second.tombstone; }

Status MemtableSource::advance() {
    ++it_;
    return {};
}

// --- TableSource -----------------------------------------------------------

Result<std::unique_ptr<TableSource>> TableSource::create(std::shared_ptr<const SSTable> table,
                                                         std::string_view begin,
                                                         std::string_view end) {
    const SSTable& reader = *table;
    std::unique_ptr<TableSource> source{new TableSource{std::move(table)}};
    source->upper_ = std::string{end};

    const std::vector<sstable::IndexEntry>& index = reader.index();
    if (index.empty()) {
        return source;  // Exhausted from the start.
    }

    // The first block that could hold `begin`: the first whose last key is >= it.
    // Same search as a point lookup, for the same reason -- earlier blocks hold
    // only smaller keys.
    std::size_t first_block = 0;
    if (!begin.empty()) {
        const auto it =
            std::lower_bound(index.begin(), index.end(), begin,
                             [](const sstable::IndexEntry& entry, std::string_view target) {
                                 return std::string_view{entry.key} < target;
                             });
        if (it == index.end()) {
            return source;  // Every key in this table is smaller than `begin`.
        }
        first_block = static_cast<std::size_t>(it - index.begin());
    }

    KVSTORE_RETURN_IF_ERROR(source->load_block(first_block));

    // Skip past any entries in that block that fall before `begin`. Only the
    // first block can contain such entries, so this happens once.
    if (!begin.empty()) {
        const auto hit =
            std::lower_bound(source->entries_.begin(), source->entries_.end(), begin,
                             [](const sstable::BlockEntry& entry, std::string_view target) {
                                 return entry.key < target;
                             });
        source->entry_index_ = static_cast<std::size_t>(hit - source->entries_.begin());

        // Unreachable with a well-formed table -- the block was chosen because
        // its last key is >= begin, so the search above must land on something.
        // Handled anyway, because "well-formed" here means the index agrees with
        // the block contents, and nothing has verified that: the index has its
        // own crc and the block has its own, and neither checks the other.
        if (source->entry_index_ >= source->entries_.size()) {
            if (source->block_index_ + 1 >= reader.block_count()) {
                source->exhausted_ = true;
                return source;
            }
            KVSTORE_RETURN_IF_ERROR(source->load_block(source->block_index_ + 1));
        }
    }

    source->apply_upper_bound();
    return source;
}

Status TableSource::load_block(std::size_t index) {
    auto block = table_->read_block(index);
    if (!block.is_ok()) {
        return block.status();
    }
    block_ = block.take();
    // entries_ holds views into block_, so it is only ever valid immediately
    // after this parse. Nothing else may touch block_. Under mmap block_ is
    // itself a view into the table's mapping, so the chain of borrowing runs one
    // link deeper -- and the table outliving this source is what holds it up.
    KVSTORE_RETURN_IF_ERROR(sstable::parse_block(block_.bytes(), &entries_));

    block_index_ = index;
    entry_index_ = 0;
    exhausted_ = entries_.empty();
    return {};
}

void TableSource::apply_upper_bound() {
    if (exhausted_ || upper_.empty()) {
        return;
    }
    if (entries_[entry_index_].key >= upper_) {
        exhausted_ = true;
    }
}

bool TableSource::valid() const { return !exhausted_; }

std::string_view TableSource::key() const { return entries_[entry_index_].key; }

std::string_view TableSource::value() const { return entries_[entry_index_].value; }

bool TableSource::tombstone() const { return entries_[entry_index_].tombstone; }

Status TableSource::advance() {
    if (exhausted_) {
        return {};
    }
    ++entry_index_;
    if (entry_index_ >= entries_.size()) {
        if (block_index_ + 1 >= table_->block_count()) {
            exhausted_ = true;
            return {};
        }
        KVSTORE_RETURN_IF_ERROR(load_block(block_index_ + 1));
    }
    apply_upper_bound();
    return {};
}

// --- MergingIterator -------------------------------------------------------

Status MergingIterator::find_next_live() {
    valid_ = false;

    for (;;) {
        // The smallest key among the sources still running, and the newest
        // source holding it. Sources arrive newest-first and the comparison is
        // strictly less-than, so the first source to reach a given minimum keeps
        // it -- which resolves the tie in favour of the newer source without
        // needing to know anything about versions or timestamps.
        const EntrySource* winner = nullptr;
        for (const std::unique_ptr<EntrySource>& source : sources_) {
            if (!source->valid()) {
                continue;
            }
            if (winner == nullptr || source->key() < winner->key()) {
                winner = source.get();
            }
        }
        if (winner == nullptr) {
            return {};  // Everything is exhausted; valid_ stays false.
        }

        // Copy before advancing. These views point into the winning source's
        // block buffer, and the next thing this loop does is move that source --
        // which may load a different block over the bytes they point at.
        key_.assign(winner->key());
        const bool deleted = winner->tombstone();
        value_.clear();
        if (!deleted) {
            value_.assign(winner->value());
        }

        // Advance *every* source sitting on this key, not just the winner. The
        // older ones hold superseded values for it, and leaving them in place
        // would hand them back on the following step -- turning an overwrite
        // into two rows and, for a tombstone, undoing the delete outright.
        for (const std::unique_ptr<EntrySource>& source : sources_) {
            if (source->valid() && source->key() == key_) {
                Status advanced = source->advance();
                if (!advanced.is_ok()) {
                    status_ = advanced;
                    return advanced;
                }
            }
        }

        // A tombstone that a reader would skip is one a compaction may still
        // have to carry forward -- so whether this ends the search is the
        // caller's choice, not the merge's.
        if (!deleted || mode_ == Tombstones::Emit) {
            tombstone_ = deleted;
            valid_ = true;
            return {};
        }
        // Skip mode, and a tombstone won: the key is deleted, so it is not a row
        // at all. Every source that had it has been advanced past it already, so
        // simply go round again.
    }
}

// --- Iterator (the public face, pimpl'd onto MergingIterator) --------------

Iterator::Iterator(std::unique_ptr<MergingIterator> impl) : impl_(std::move(impl)) {}

// Out of line, because MergingIterator is incomplete in the public header.
Iterator::~Iterator() = default;
Iterator::Iterator(Iterator&&) noexcept = default;
Iterator& Iterator::operator=(Iterator&&) noexcept = default;

bool Iterator::valid() const { return impl_ != nullptr && impl_->valid(); }

std::string_view Iterator::key() const { return impl_->key(); }

std::string_view Iterator::value() const { return impl_->value(); }

Status Iterator::next() { return impl_->next(); }

Status Iterator::status() const { return impl_ == nullptr ? Status::ok() : impl_->status(); }

}  // namespace kvstore
