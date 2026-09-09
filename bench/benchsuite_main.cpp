// kv-benchsuite -- comparative measurements of the storage engine, as CSV.
//
// Separate from kv_bench, which answers a different question. kv_bench prints
// one human-readable snapshot of amplification for a single configuration;
// this one produces rows meant to be compared -- across a toggle, across a
// build, across a machine, across a month. That difference is what every piece
// of methodology below is for:
//
//   * A warm-up pass before every measured run, discarded. The first sweep over
//     a fresh store measures page-cache misses and lazily-built state, which is
//     a real cost but not the one being reported.
//   * steady_clock around the measured region only -- never around setup,
//     never around teardown.
//   * Five runs, all five emitted, plus a median row. Reporting only the median
//     hides the spread; reporting only the raw rows makes the reader do
//     arithmetic. Both costs nothing.
//   * The environment in every row, because a number without its machine is an
//     anecdote (see bench_env.hpp).
//   * A refusal to start if the data directory is not ext4.
//   * Every measured operation's Status is checked. kv_bench discards them, so
//     a run where every get() failed would report a magnificent throughput.
//
// Two toggles this measures did not exist before it: LsmOptions::bloom_enabled
// and BitcaskOptions::ignore_hints. Both default to the engine's existing
// behaviour and are documented where they are declared.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "bench_env.hpp"
#include "kvstore/bitcask.hpp"
#include "kvstore/lsm.hpp"
#include "kvstore/status.hpp"

namespace {

// --- Fixed workload shape ---------------------------------------------------

// 16 bytes exactly: "key_" plus 12 digits. Fixed rather than configurable
// because the point of this harness is comparability, and a key size that
// drifts between runs quietly changes the index's memory footprint, the
// per-entry overhead, and how many entries fit in a block.
constexpr std::size_t kKeyBytes = 16;
constexpr std::size_t kDefaultValueBytes = 100;

// The secondary size for the recovery comparison.
//
// Hint files record keys and record locations, never values, so what a hint
// saves is exactly the value bytes -- which means the whole benefit scales with
// value size and vanishes as values approach the key size. At 100-byte values
// the hint is roughly a third of the data file and the measured end-to-end win
// is small, because rebuilding the index costs the same either way. NOTES.md's
// 3.5x was measured at 8KiB, so that is the second point plotted here.
constexpr std::size_t kRecoverySecondaryValueBytes = 8192;

// The memtable charges key + value + 64 per entry (src/memtable.cpp:10-12);
// on-disk cost differs again. This is the *user* byte count -- what the caller
// stored -- which is the only figure that means the same thing for both
// engines.
constexpr std::uint64_t user_bytes_per_entry(std::size_t value_size) {
    return kKeyBytes + value_size;
}

// Dataset size is held constant in *bytes*, so a larger value size means
// proportionally fewer records. That is the comparison the recovery benchmark
// wants: the same volume of data on disk, reorganised into fewer, fatter
// records, where the hint file shrinks but the data file does not.
constexpr std::uint64_t keys_for(std::uint64_t dataset_mb, std::size_t value_size) {
    return (dataset_mb * 1024ull * 1024ull) / user_bytes_per_entry(value_size);
}

// Present keys get even suffixes, absent keys odd ones.
//
// This is not arbitrary. An absent key outside a table's [min_key, max_key] is
// rejected by the range check in SSTable::lookup() before the filter is ever
// consulted, so the obvious choice -- probe for keys past the end of the
// dataset -- measures a range comparison and reports it as bloom performance.
// Interleaving guarantees every absent probe falls strictly between two present
// keys, inside every table's range, and therefore actually reaches the filter.
//
// kv_bench solves the same problem by appending "z" to a real key, which makes
// the key 17 bytes; the even/odd split keeps every key exactly 16.
std::string present_key(std::uint64_t index) {
    char buffer[kKeyBytes + 1];
    std::snprintf(buffer, sizeof(buffer), "key_%012llu",
                  static_cast<unsigned long long>(index * 2));
    return std::string(buffer, kKeyBytes);
}

std::string absent_key(std::uint64_t index) {
    char buffer[kKeyBytes + 1];
    std::snprintf(buffer, sizeof(buffer), "key_%012llu",
                  static_cast<unsigned long long>(index * 2 + 1));
    return std::string(buffer, kKeyBytes);
}

// --- Arguments --------------------------------------------------------------

enum class Engine { Bitcask, Lsm };
enum class Workload { ReadPresent, ReadAbsent, WriteBatched, Recovery, All };

struct Args {
    Engine engine = Engine::Lsm;
    Workload workload = Workload::All;

    bool bloom_enabled = true;
    bool ignore_hints = false;
    kvstore::SyncMode sync_mode = kvstore::SyncMode::Never;

    std::uint64_t dataset_mb = 1500;
    std::size_t value_size = kDefaultValueBytes;

    // A second recovery pass at this value size, emitted as its own labelled
    // rows. 0 disables it. Only the recovery workload uses it -- the read and
    // write numbers are meant to stay on one fixed shape.
    std::size_t recovery_secondary_value_size = kRecoverySecondaryValueBytes;

    std::uint64_t ops = 1000000;
    std::uint64_t warmup_ops = 0;  // 0 means "same as ops".
    int runs = 5;
    std::uint64_t batch_size = 1000;

    // 0 on the command line means "engine default"; parse_args resolves it to
    // the real value so the CSV never reports the sentinel.
    std::uint64_t memtable_size = 0;
    std::uint32_t bits_per_key = 0;

    std::uint64_t seed = 20260824;
    std::filesystem::path dir;

    bool reuse_dir = false;
    bool allow_non_ext4 = false;
    bool no_header = false;
    bool force = false;
    bool help = false;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: kv-benchsuite --dir PATH [options]\n"
                 "\n"
                 "Emits CSV to stdout. Keys are 16 bytes, values 100 bytes, access\n"
                 "uniform random. Every configuration runs --runs times; all runs are\n"
                 "emitted plus a median row.\n"
                 "\n"
                 "  --dir PATH            data directory (required; must be ext4)\n"
                 "  --engine NAME         bitcask or lsm (default: lsm)\n"
                 "  --workload NAME       read-present, read-absent, write-batched,\n"
                 "                        recovery, or all (default: all)\n"
                 "\n"
                 "  --bloom on|off        consult bloom filters on reads (LSM; default on).\n"
                 "                        Filters are built either way -- only the probe is\n"
                 "                        skipped, so both arms read identical files.\n"
                 "  --recovery MODE       hints or full-replay (Bitcask; default hints).\n"
                 "                        full-replay ignores the hint files beside the data.\n"
                 "\n"
                 "  --dataset-mb N        user bytes to load, in MiB (default: 1500)\n"
                 "  --value-size N        value bytes (default: 100). Dataset size is held\n"
                 "                        constant in bytes, so a bigger value means fewer\n"
                 "                        records. Keys are always 16 bytes.\n"
                 "  --recovery-secondary-value-size N\n"
                 "                        emit a second recovery pass at this value size\n"
                 "                        (default: 8192; 0 disables). Hints save value bytes\n"
                 "                        only, so the benefit scales with value size.\n"
                 "  --ops N               measured operations per run (default: 1000000)\n"
                 "  --warmup-ops N        discarded operations before each run (default: --ops)\n"
                 "  --runs N              repetitions (default: 5)\n"
                 "  --batch-size N        writes per explicit sync() (default: 1000)\n"
                 "\n"
                 "  --sync always|never   fsync per write, or not (default: never)\n"
                 "  --memtable-size N     LSM memtable flush threshold, bytes\n"
                 "  --bits-per-key N      LSM bloom bits per key (default: 10)\n"
                 "  --seed N              RNG seed (default: fixed, for reproducibility)\n"
                 "\n"
                 "  --reuse-dir           keep an existing dataset instead of rebuilding\n"
                 "  --allow-non-ext4      proceed on a non-ext4 filesystem (not advised)\n"
                 "  --no-header           omit the CSV header, for appending\n"
                 "  --force               run even from a non-Release build\n"
                 "  --help\n");
}

bool parse_args(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            args->help = true;
            return true;
        }
        if (arg == "--reuse-dir") {
            args->reuse_dir = true;
        } else if (arg == "--allow-non-ext4") {
            args->allow_non_ext4 = true;
        } else if (arg == "--no-header") {
            args->no_header = true;
        } else if (arg == "--force") {
            args->force = true;
        } else if (arg == "--dir" && has_value) {
            args->dir = argv[++i];
        } else if (arg == "--engine" && has_value) {
            const std::string name = argv[++i];
            if (name == "bitcask") {
                args->engine = Engine::Bitcask;
            } else if (name == "lsm") {
                args->engine = Engine::Lsm;
            } else {
                std::fprintf(stderr, "kv-benchsuite: unknown engine: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--workload" && has_value) {
            const std::string name = argv[++i];
            if (name == "read-present") {
                args->workload = Workload::ReadPresent;
            } else if (name == "read-absent") {
                args->workload = Workload::ReadAbsent;
            } else if (name == "write-batched") {
                args->workload = Workload::WriteBatched;
            } else if (name == "recovery") {
                args->workload = Workload::Recovery;
            } else if (name == "all") {
                args->workload = Workload::All;
            } else {
                std::fprintf(stderr, "kv-benchsuite: unknown workload: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--bloom" && has_value) {
            const std::string name = argv[++i];
            if (name == "on") {
                args->bloom_enabled = true;
            } else if (name == "off") {
                args->bloom_enabled = false;
            } else {
                std::fprintf(stderr, "kv-benchsuite: --bloom takes on or off: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--recovery" && has_value) {
            const std::string name = argv[++i];
            if (name == "hints") {
                args->ignore_hints = false;
            } else if (name == "full-replay") {
                args->ignore_hints = true;
            } else {
                std::fprintf(stderr,
                             "kv-benchsuite: --recovery takes hints or full-replay: %s\n",
                             name.c_str());
                return false;
            }
        } else if (arg == "--sync" && has_value) {
            const std::string name = argv[++i];
            if (name == "always") {
                args->sync_mode = kvstore::SyncMode::Always;
            } else if (name == "never") {
                args->sync_mode = kvstore::SyncMode::Never;
            } else {
                std::fprintf(stderr, "kv-benchsuite: --sync takes always or never: %s\n",
                             name.c_str());
                return false;
            }
        } else if (arg == "--dataset-mb" && has_value) {
            args->dataset_mb = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--value-size" && has_value) {
            args->value_size = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--recovery-secondary-value-size" && has_value) {
            args->recovery_secondary_value_size =
                static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--ops" && has_value) {
            args->ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--warmup-ops" && has_value) {
            args->warmup_ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--runs" && has_value) {
            args->runs = std::atoi(argv[++i]);
        } else if (arg == "--batch-size" && has_value) {
            args->batch_size = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--memtable-size" && has_value) {
            args->memtable_size = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--bits-per-key" && has_value) {
            args->bits_per_key = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--seed" && has_value) {
            args->seed = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "kv-benchsuite: unrecognised argument: %s\n", arg.c_str());
            return false;
        }
    }

    if (args->dir.empty()) {
        std::fprintf(stderr, "kv-benchsuite: --dir is required\n");
        return false;
    }
    if (args->dataset_mb == 0 || args->ops == 0 || args->runs <= 0 || args->batch_size == 0) {
        std::fprintf(stderr,
                     "kv-benchsuite: --dataset-mb, --ops, --runs and --batch-size "
                     "must all be positive\n");
        return false;
    }
    if (args->value_size == 0) {
        std::fprintf(stderr, "kv-benchsuite: --value-size must be positive\n");
        return false;
    }
    if (keys_for(args->dataset_mb, args->value_size) == 0) {
        std::fprintf(stderr,
                     "kv-benchsuite: --dataset-mb %llu holds no records at --value-size %zu\n",
                     static_cast<unsigned long long>(args->dataset_mb), args->value_size);
        return false;
    }
    if (args->warmup_ops == 0) {
        args->warmup_ops = args->ops;
    }

    // Resolve "0 means engine default" into the value actually used, here, once.
    // A CSV that reported the sentinel would say memtable_size=0 for every
    // default run -- which is not a memtable size, and is exactly the kind of
    // column a reader would later try to plot.
    const kvstore::LsmOptions defaults;
    if (args->memtable_size == 0) {
        args->memtable_size = defaults.memtable_size;
    }
    if (args->bits_per_key == 0) {
        args->bits_per_key = defaults.bits_per_key;
    }
    return true;
}

// --- Engine handling --------------------------------------------------------

// Both concrete types are kept, not just the KVStore pointer: settle() and the
// recovery workload need sync()/flush()/compact(), and none of those are on the
// interface -- deliberately, since none would mean anything for a MemoryStore.
struct Harness {
    std::unique_ptr<kvstore::KVStore> store;
    kvstore::Bitcask* bitcask = nullptr;
    kvstore::LsmStore* lsm = nullptr;

    // Quiesce background work so a measured phase does not race a compaction
    // started by the load phase. Without this the first run absorbs work the
    // other four do not, and run 1 is always the slow one.
    void settle() {
        if (lsm != nullptr) {
            (void)lsm->flush();
            (void)lsm->wait_for_background();
        } else if (bitcask != nullptr) {
            (void)bitcask->sync();
        }
    }

    void sync() {
        if (lsm != nullptr) {
            (void)lsm->sync();
        } else if (bitcask != nullptr) {
            (void)bitcask->sync();
        }
    }
};

kvstore::BitcaskOptions bitcask_options(const Args& args) {
    kvstore::BitcaskOptions options;
    options.sync_mode = args.sync_mode;
    options.ignore_hints = args.ignore_hints;
    return options;
}

kvstore::LsmOptions lsm_options(const Args& args) {
    kvstore::LsmOptions options;
    options.sync_mode = args.sync_mode;
    options.bloom_enabled = args.bloom_enabled;
    // Never the 0 sentinel by this point -- parse_args resolved it.
    options.memtable_size = args.memtable_size;
    options.bits_per_key = args.bits_per_key;
    return options;
}

Harness open_engine(const Args& args) {
    Harness harness;
    if (args.engine == Engine::Bitcask) {
        auto opened = kvstore::Bitcask::open(args.dir, bitcask_options(args));
        if (!opened.is_ok()) {
            std::fprintf(stderr, "kv-benchsuite: open: %s\n",
                         opened.status().to_string().c_str());
            std::exit(1);
        }
        auto store = opened.take();
        harness.bitcask = store.get();
        harness.store = std::move(store);
        return harness;
    }

    auto opened = kvstore::LsmStore::open(args.dir, lsm_options(args));
    if (!opened.is_ok()) {
        std::fprintf(stderr, "kv-benchsuite: open: %s\n", opened.status().to_string().c_str());
        std::exit(1);
    }
    auto store = opened.take();
    harness.lsm = store.get();
    harness.store = std::move(store);
    return harness;
}

// --- CSV --------------------------------------------------------------------

struct Row {
    std::string workload;
    std::string run;  // "1".."N", or "median".
    std::uint64_t ops = 0;
    double elapsed_s = 0.0;
    double throughput = 0.0;
    double p50_us = 0.0;
    double p99_us = 0.0;
    double p999_us = 0.0;
};

struct Context {
    kvbench::Env env;
    Args args;
    std::uint64_t keys = 0;

    // Not read from args: the secondary recovery pass runs with a different one
    // and reports it in its own rows, so the value size travels with the phase
    // rather than with the command line.
    std::size_t value_size = kDefaultValueBytes;

    double clock_ns = 0.0;
    std::string timestamp;
};

// Anything that could contain a comma is quoted; CPU model names do.
std::string csv_quote(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
    return out;
}

void print_header() {
    std::printf(
        "timestamp,host,kernel,cpu_model,cores,threads,ram_gb,fs_type,build_type,cache,"
        "engine,workload,bloom_enabled,ignore_hints,sync_mode,batch_size,memtable_size,"
        "bits_per_key,keys,key_bytes,value_bytes,dataset_bytes,run,ops,elapsed_s,"
        "throughput_ops_s,p50_us,p99_us,p999_us,clock_overhead_ns\n");
}

void print_row(const Context& ctx, const Row& row) {
    const Args& a = ctx.args;
    const bool is_lsm = a.engine == Engine::Lsm;

    // Columns that mean nothing for the engine being measured are left empty
    // rather than filled with a plausible-looking zero. Bitcask has no memtable,
    // no bloom filter and no read-time bloom switch; Bitcask's hint files have
    // no counterpart in the LSM engine. A reader sorting by these columns should
    // find blanks, not values that invite a comparison that cannot be made.
    char memtable[32] = "";
    char bits[16] = "";
    char bloom[4] = "";
    char hints[4] = "";
    if (is_lsm) {
        std::snprintf(memtable, sizeof(memtable), "%llu",
                      static_cast<unsigned long long>(a.memtable_size));
        std::snprintf(bits, sizeof(bits), "%u", a.bits_per_key);
        std::snprintf(bloom, sizeof(bloom), "%d", a.bloom_enabled ? 1 : 0);
    } else {
        std::snprintf(hints, sizeof(hints), "%d", a.ignore_hints ? 1 : 0);
    }

    std::printf(
        "%s,%s,%s,%s,%u,%u,%.2f,%s,%s,warm,"
        "%s,%s,%s,%s,%s,%llu,%s,"
        "%s,%llu,%zu,%zu,%llu,%s,%llu,%.6f,"
        "%.1f,%.3f,%.3f,%.3f,%.1f\n",
        ctx.timestamp.c_str(), csv_quote(ctx.env.hostname).c_str(),
        csv_quote(ctx.env.kernel).c_str(), csv_quote(ctx.env.cpu_model).c_str(), ctx.env.cores,
        ctx.env.threads, static_cast<double>(ctx.env.ram_bytes) / (1024.0 * 1024.0 * 1024.0),
        ctx.env.fs_type.c_str(), KVBENCH_BUILD_TYPE,
        is_lsm ? "lsm" : "bitcask", row.workload.c_str(), bloom, hints,
        a.sync_mode == kvstore::SyncMode::Always ? "always" : "never",
        static_cast<unsigned long long>(a.batch_size), memtable,
        bits, static_cast<unsigned long long>(ctx.keys), kKeyBytes, ctx.value_size,
        static_cast<unsigned long long>(ctx.keys * user_bytes_per_entry(ctx.value_size)),
        row.run.c_str(),
        static_cast<unsigned long long>(row.ops), row.elapsed_s, row.throughput, row.p50_us,
        row.p99_us, row.p999_us, ctx.clock_ns);
}

// The five raw rows, then the median of each column.
//
// The median row's percentiles are medians *of the per-run percentiles*, not a
// percentile over the pooled samples. That is the honest reading of
// "median-of-5": it answers "what does a typical run's p99 look like", where
// pooling would answer a different question and hide a single pathological run.
void emit(const Context& ctx, const std::string& workload, std::vector<Row>& runs) {
    std::vector<double> elapsed;
    std::vector<double> throughput;
    std::vector<double> p50;
    std::vector<double> p99;
    std::vector<double> p999;

    for (std::size_t i = 0; i < runs.size(); ++i) {
        runs[i].workload = workload;
        runs[i].run = std::to_string(i + 1);
        print_row(ctx, runs[i]);
        elapsed.push_back(runs[i].elapsed_s);
        throughput.push_back(runs[i].throughput);
        p50.push_back(runs[i].p50_us);
        p99.push_back(runs[i].p99_us);
        p999.push_back(runs[i].p999_us);
    }

    Row summary;
    summary.workload = workload;
    summary.run = "median";
    summary.ops = runs.empty() ? 0 : runs.front().ops;
    summary.elapsed_s = kvbench::median(elapsed);
    summary.throughput = kvbench::median(throughput);
    summary.p50_us = kvbench::median(p50);
    summary.p99_us = kvbench::median(p99);
    summary.p999_us = kvbench::median(p999);
    print_row(ctx, summary);
    std::fflush(stdout);
}

Row summarise(std::vector<double>& samples_us, double elapsed_s, std::uint64_t ops) {
    std::sort(samples_us.begin(), samples_us.end());
    Row row;
    row.ops = ops;
    row.elapsed_s = elapsed_s;
    row.throughput = elapsed_s > 0.0 ? static_cast<double>(ops) / elapsed_s : 0.0;
    row.p50_us = kvbench::percentile(samples_us, 0.50);
    row.p99_us = kvbench::percentile(samples_us, 0.99);
    row.p999_us = kvbench::percentile(samples_us, 0.999);
    return row;
}

[[noreturn]] void fail_op(const char* what, const kvstore::Status& status) {
    std::fprintf(stderr, "kv-benchsuite: %s failed: %s\n", what, status.to_string().c_str());
    std::exit(1);
}

// --- Dataset ----------------------------------------------------------------

void load_dataset(Harness& harness, std::uint64_t keys, const std::string& value) {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < keys; ++i) {
        const kvstore::Status status = harness.store->put(present_key(i), value);
        if (!status.is_ok()) {
            fail_op("load put", status);
        }
    }
    harness.settle();
    const auto stop = std::chrono::steady_clock::now();
    std::fprintf(stderr, "kv-benchsuite: loaded %llu keys in %.1fs\n",
                 static_cast<unsigned long long>(keys),
                 std::chrono::duration<double>(stop - start).count());
}

// --- Workloads --------------------------------------------------------------

// Reads, present or absent. `expect_found` decides which key space is probed
// and what the correct answer is; an unexpected answer aborts, because a get()
// that silently returned NotFound for every key would otherwise be reported as
// excellent throughput.
Row measure_reads(Harness& harness, const Context& ctx, bool expect_found) {
    const Args& a = ctx.args;
    std::mt19937_64 rng(a.seed);
    std::uniform_int_distribution<std::uint64_t> pick(0, ctx.keys - 1);
    std::string value;

    // Warm-up, discarded. Same distribution, same work, no samples kept.
    for (std::uint64_t i = 0; i < a.warmup_ops; ++i) {
        const std::uint64_t index = pick(rng);
        (void)harness.store->get(expect_found ? present_key(index) : absent_key(index), &value);
    }

    // Reserved before the timed region: a reallocation inside it would land in
    // whichever sample happened to trigger it and show up as a latency spike
    // that belongs to the harness, not the engine.
    std::vector<double> samples;
    samples.reserve(a.ops);

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < a.ops; ++i) {
        const std::uint64_t index = pick(rng);
        const std::string key = expect_found ? present_key(index) : absent_key(index);

        const auto op_start = std::chrono::steady_clock::now();
        const kvstore::Status status = harness.store->get(key, &value);
        const auto op_stop = std::chrono::steady_clock::now();

        if (expect_found) {
            if (!status.is_ok()) {
                fail_op("get (expected present)", status);
            }
        } else if (!status.is_not_found()) {
            fail_op("get (expected absent)", status);
        }
        samples.push_back(std::chrono::duration<double, std::micro>(op_stop - op_start).count());
    }
    const auto stop = std::chrono::steady_clock::now();

    return summarise(samples, std::chrono::duration<double>(stop - start).count(), a.ops);
}

// Batched writes.
//
// SyncMode has exactly two values, Always and Never -- there is no batched or
// group-commit mode in the engine. So a batch here is emulated: run with the
// per-write fsync off and call sync() explicitly every --batch-size writes. The
// CSV records the batch size so the emulation is visible rather than implied.
// Note the two engines sync different things: Bitcask::sync() flushes the
// active data file, LsmStore::sync() flushes the WAL.
Row measure_writes(Harness& harness, const Context& ctx) {
    const Args& a = ctx.args;
    const std::string value(ctx.value_size, 'x');
    std::mt19937_64 rng(a.seed);
    std::uniform_int_distribution<std::uint64_t> pick(0, ctx.keys - 1);

    for (std::uint64_t i = 0; i < a.warmup_ops; ++i) {
        (void)harness.store->put(present_key(pick(rng)), value);
    }
    harness.sync();

    std::vector<double> samples;
    samples.reserve(a.ops);

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < a.ops; ++i) {
        const std::string key = present_key(pick(rng));

        const auto op_start = std::chrono::steady_clock::now();
        const kvstore::Status status = harness.store->put(key, value);
        if ((i + 1) % a.batch_size == 0) {
            harness.sync();
        }
        const auto op_stop = std::chrono::steady_clock::now();

        if (!status.is_ok()) {
            fail_op("put", status);
        }
        samples.push_back(std::chrono::duration<double, std::micro>(op_stop - op_start).count());
    }
    harness.sync();
    const auto stop = std::chrono::steady_clock::now();

    return summarise(samples, std::chrono::duration<double>(stop - start).count(), a.ops);
}

// Recovery: how long open() takes on a populated directory.
//
// One sample per run, so the percentile columns are all that single value --
// which is correct, not a placeholder: p50 and p99 of one observation are that
// observation. The median row across five runs is the number to read.
//
// These are warm-cache figures. Dropping the page cache needs
// `sudo sysctl vm.drop_caches=3`, which is out of scope here, so this measures
// replay and index-rebuild CPU rather than cold disk I/O. Every row says
// cache=warm so a future cold run stays distinguishable.
Row measure_recovery(const Context& ctx) {
    const Args& a = ctx.args;

    // Warm-up open/close, discarded.
    {
        Harness warm = open_engine(a);
        warm.settle();
        warm.store.reset();
    }

    std::vector<double> samples;
    samples.reserve(1);

    const auto start = std::chrono::steady_clock::now();
    Harness harness = open_engine(a);
    const auto stop = std::chrono::steady_clock::now();

    const double elapsed_s = std::chrono::duration<double>(stop - start).count();
    samples.push_back(elapsed_s * 1e6);

    harness.store.reset();
    return summarise(samples, elapsed_s, 1);
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) {
        print_usage();
        return 2;
    }
    if (args.help) {
        print_usage();
        return 0;
    }

    // Release-only, enforced here rather than by omitting the target, so the
    // debug tree still compiles this file and the guard itself gets built.
#if defined(__SANITIZE_ADDRESS__)
    if (!args.force) {
        std::fprintf(stderr,
                     "kv-benchsuite: refusing to run: built with AddressSanitizer, which "
                     "costs 2-3x.\n  Use the Release build, or --force.\n");
        return 2;
    }
#endif
#if !defined(NDEBUG)
    if (!args.force) {
        std::fprintf(stderr,
                     "kv-benchsuite: refusing to run: this is a debug build (NDEBUG unset).\n"
                     "  Use the Release build, or --force.\n");
        return 2;
    }
#endif
    if (!args.force && std::strcmp(KVBENCH_BUILD_TYPE, "Release") != 0) {
        std::fprintf(stderr,
                     "kv-benchsuite: refusing to run: build type is %s, not Release.\n"
                     "  Use the Release build, or --force.\n",
                     KVBENCH_BUILD_TYPE);
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(args.dir, ec);
    if (ec) {
        std::fprintf(stderr, "kv-benchsuite: cannot create %s: %s\n", args.dir.string().c_str(),
                     ec.message().c_str());
        return 1;
    }

    // Before anything expensive: a run on 9p is not worth the hour it takes.
    kvbench::assert_ext4(args.dir, args.allow_non_ext4);

    Context ctx;
    ctx.args = args;
    ctx.env = kvbench::capture_env(args.dir);
    ctx.value_size = args.value_size;
    ctx.keys = keys_for(args.dataset_mb, args.value_size);

    {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", std::gmtime(&now));
        ctx.timestamp = stamp;
    }

    std::fprintf(stderr,
                 "kv-benchsuite: measuring clock overhead...\n");
    ctx.clock_ns = kvbench::clock_overhead_ns();
    std::fprintf(stderr, "kv-benchsuite: steady_clock::now() = %.1f ns\n", ctx.clock_ns);

    if (!args.no_header) {
        print_header();
    }

    const std::string value(ctx.value_size, 'x');
    const bool want_reads =
        args.workload == Workload::All || args.workload == Workload::ReadPresent ||
        args.workload == Workload::ReadAbsent;

    // --- Read workloads -----------------------------------------------------
    //
    // One dataset serves every read run and both bloom arms. That is the whole
    // reason bloom_enabled is a read-time flag: rebuilding between arms would
    // confound the probe's cost with a different on-disk layout.
    if (want_reads) {
        if (!args.reuse_dir) {
            std::filesystem::remove_all(args.dir, ec);
            std::filesystem::create_directories(args.dir, ec);
        }
        Harness harness = open_engine(args);
        if (!args.reuse_dir) {
            load_dataset(harness, ctx.keys, value);
        }
        harness.settle();

        if (args.workload == Workload::All || args.workload == Workload::ReadPresent) {
            std::vector<Row> runs;
            for (int run = 0; run < args.runs; ++run) {
                runs.push_back(measure_reads(harness, ctx, true));
            }
            emit(ctx, "read_present", runs);
        }
        if (args.workload == Workload::All || args.workload == Workload::ReadAbsent) {
            std::vector<Row> runs;
            for (int run = 0; run < args.runs; ++run) {
                runs.push_back(measure_reads(harness, ctx, false));
            }
            emit(ctx, "read_absent", runs);
        }

        // Released before anything else touches the directory: on Windows a
        // file cannot be unlinked while a handle or mapping is live, and the
        // LSM destructor also has to join its background thread first.
        harness.store.reset();
    }

    // --- Batched writes -----------------------------------------------------
    //
    // A fresh directory per run: writing into the store left by the previous
    // run would make each successive run an overwrite workload against a bigger
    // database, so run 5 would not be comparable with run 1.
    if (args.workload == Workload::All || args.workload == Workload::WriteBatched) {
        std::vector<Row> runs;
        for (int run = 0; run < args.runs; ++run) {
            std::filesystem::remove_all(args.dir, ec);
            std::filesystem::create_directories(args.dir, ec);
            Harness harness = open_engine(args);
            load_dataset(harness, ctx.keys, value);
            runs.push_back(measure_writes(harness, ctx));
            harness.store.reset();
        }
        emit(ctx, "write_batched", runs);
    }

    // --- Recovery -----------------------------------------------------------
    //
    // Run at two value sizes, because the hint-file benefit is not a single
    // number. A hint records keys and record locations but never values, so it
    // saves exactly the value bytes: at 100 bytes it saves little against the
    // cost of rebuilding the index, and at 8KiB it saves a great deal. Reporting
    // only the 100-byte figure would understate hints; only the 8KiB figure
    // would oversell them. Both rows carry their own value_bytes column.
    if (args.workload == Workload::All || args.workload == Workload::Recovery) {
        const auto recovery_phase = [&](std::size_t value_size, const char* label) {
            Context phase = ctx;
            phase.value_size = value_size;
            phase.keys = keys_for(args.dataset_mb, value_size);
            if (phase.keys == 0) {
                std::fprintf(stderr,
                             "kv-benchsuite: skipping %s: --dataset-mb %llu holds no records "
                             "at %zu-byte values\n",
                             label, static_cast<unsigned long long>(args.dataset_mb), value_size);
                return;
            }
            const std::string phase_value(value_size, 'x');

            std::vector<Row> runs;
            for (int run = 0; run < args.runs; ++run) {
                std::filesystem::remove_all(args.dir, ec);
                std::filesystem::create_directories(args.dir, ec);
                {
                    Harness harness = open_engine(args);
                    load_dataset(harness, phase.keys, phase_value);

                    // Bitcask writes hint files only during compaction, never on
                    // rotation. Without this call a freshly loaded database has
                    // no hints at all, both --recovery arms would do a full
                    // scan, and the comparison would silently measure nothing.
                    if (harness.bitcask != nullptr) {
                        const kvstore::Status status = harness.bitcask->compact();
                        if (!status.is_ok()) {
                            fail_op("compact", status);
                        }
                    }
                    harness.settle();
                    harness.store.reset();
                }
                runs.push_back(measure_recovery(phase));
            }
            emit(phase, label, runs);
        };

        recovery_phase(ctx.value_size, "recovery");

        // Secondary pass. Skipped when disabled, and when it would only repeat
        // the primary at the same size.
        if (args.recovery_secondary_value_size != 0 &&
            args.recovery_secondary_value_size != ctx.value_size) {
            recovery_phase(args.recovery_secondary_value_size, "recovery_large_value");
        }
    }

    return 0;
}
