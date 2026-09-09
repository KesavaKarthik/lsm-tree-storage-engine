#pragma once

#include <atomic>
#include <cstdint>

namespace kvstore {

// What the LSM engine did, counted.
//
// These are not decoration. Two of the three amplification factors an LSM is
// judged on are only observable from inside the engine -- read amplification is
// "how many blocks did that get() actually touch", and no test or benchmark can
// see it from the outside. Counting them here is what turns "the bloom filter
// works" from a claim into an assertion (Step 2) and what lets bench/ report
// amplification rather than just throughput (Step 3).
//
// Atomic already, while the engine is still single-threaded. The cost is a
// relaxed increment on paths that are about to do file I/O, which is
// unmeasurable; the benefit is that Step 3 turns on background threads without
// this becoming a data race ThreadSanitizer has to find for us.
struct LsmStats {
    std::atomic<std::uint64_t> gets{0};

    // Tables whose contents were consulted for a lookup: "every table until one
    // answers". Bloom filters do not reduce this -- a table is still asked -- they
    // reduce what asking *costs*, which is block_reads.
    std::atomic<std::uint64_t> tables_probed{0};

    // Data blocks actually read from disk. Read amplification, countable, and
    // the number the filters exist to drive to zero for absent keys.
    std::atomic<std::uint64_t> block_reads{0};

    // Filter consultations, and the ones that ended the search for free. The
    // ratio is the filter earning its RAM.
    std::atomic<std::uint64_t> bloom_checks{0};
    std::atomic<std::uint64_t> bloom_rejects{0};

    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> flushes{0};
    std::atomic<std::uint64_t> wal_records_replayed{0};

    // Compaction work, for the write- and space-amplification numbers bench/
    // will report in Step 3.
    std::atomic<std::uint64_t> compactions{0};
    std::atomic<std::uint64_t> compaction_bytes_read{0};
    std::atomic<std::uint64_t> compaction_bytes_written{0};
    std::atomic<std::uint64_t> tables_obsoleted{0};

    LsmStats() = default;
    LsmStats(const LsmStats&) = delete;
    LsmStats& operator=(const LsmStats&) = delete;

    void reset() noexcept {
        gets.store(0, std::memory_order_relaxed);
        tables_probed.store(0, std::memory_order_relaxed);
        block_reads.store(0, std::memory_order_relaxed);
        bloom_checks.store(0, std::memory_order_relaxed);
        bloom_rejects.store(0, std::memory_order_relaxed);
        bytes_written.store(0, std::memory_order_relaxed);
        flushes.store(0, std::memory_order_relaxed);
        wal_records_replayed.store(0, std::memory_order_relaxed);
        compactions.store(0, std::memory_order_relaxed);
        compaction_bytes_read.store(0, std::memory_order_relaxed);
        compaction_bytes_written.store(0, std::memory_order_relaxed);
        tables_obsoleted.store(0, std::memory_order_relaxed);
    }

    static void bump(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) noexcept {
        counter.fetch_add(by, std::memory_order_relaxed);
    }
};

}  // namespace kvstore
