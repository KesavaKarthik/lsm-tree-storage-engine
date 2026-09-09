// kv_bench -- throughput, latency, and the three amplifications.
//
// The numbers this prints are the point of the whole project, because they are
// the ones no test can assert and no README can claim honestly without them:
//
//   write amplification  bytes written to disk / bytes of user data. The price
//                        of leveled compaction, and it should climb toward the
//                        fanout as data grows. If it stays at 1, compaction is
//                        not running.
//   read amplification   data blocks read per get(). Near 1 for a key that is
//                        there; near **0** for one that is not, which is the
//                        bloom filter working and is measurable no other way.
//   space amplification  bytes on disk / bytes of live data. Leveled should
//                        settle near 1.1 and spike during a compaction.
//
// And the comparison the two engines exist for: Bitcask should win point reads
// outright -- one hash lookup, one seek -- while needing a pointer in RAM for
// every key it holds. The LSM should lose that race and not care.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/lsm.hpp"
#include "kvstore/status.hpp"

namespace {

enum class Engine { Bitcask, Lsm };
enum class Workload { SeqWrite, RandWrite, RandRead, Missing, Mixed };

struct Args {
    Engine engine = Engine::Lsm;
    kvstore::ReadMode read_mode = kvstore::ReadMode::Pread;
    Workload workload = Workload::RandRead;
    int keys = 200000;
    int value_size = 100;
    int threads = 1;
    std::filesystem::path dir;
    bool help = false;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: kv_bench [options]\n"
                 "\n"
                 "  --engine NAME     bitcask | lsm            (default: lsm)\n"
                 "  --read-mode NAME  pread | mmap             (default: pread)\n"
                 "  --workload NAME   seqwrite | randwrite | randread |\n"
                 "                    missing | mixed          (default: randread)\n"
                 "  --keys N          number of keys           (default: 200000)\n"
                 "  --value-size N    bytes per value          (default: 100)\n"
                 "  --threads N       concurrent readers       (default: 1)\n"
                 "  --dir PATH        where to build it        (default: a temp dir)\n"
                 "  --help\n");
}

std::string key_at(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%010d", i);
    return buf;
}

// Percentiles from a sorted sample. Nothing clever: the samples are already in
// memory and sorting a few hundred thousand doubles costs less than the run.
double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t index =
        static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

std::uint64_t directory_bytes(const std::filesystem::path& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(entry.path(), size_ec);
        if (!size_ec) {
            total += size;
        }
    }
    return total;
}

bool parse_args(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            args->help = true;
            return true;
        }
        if (arg == "--engine" && has_value) {
            const std::string name = argv[++i];
            if (name == "bitcask") {
                args->engine = Engine::Bitcask;
            } else if (name == "lsm") {
                args->engine = Engine::Lsm;
            } else {
                std::fprintf(stderr, "kv_bench: unknown engine: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--read-mode" && has_value) {
            const std::string name = argv[++i];
            if (name == "pread") {
                args->read_mode = kvstore::ReadMode::Pread;
            } else if (name == "mmap") {
                args->read_mode = kvstore::ReadMode::Mmap;
            } else {
                std::fprintf(stderr, "kv_bench: unknown read mode: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--workload" && has_value) {
            const std::string name = argv[++i];
            if (name == "seqwrite") {
                args->workload = Workload::SeqWrite;
            } else if (name == "randwrite") {
                args->workload = Workload::RandWrite;
            } else if (name == "randread") {
                args->workload = Workload::RandRead;
            } else if (name == "missing") {
                args->workload = Workload::Missing;
            } else if (name == "mixed") {
                args->workload = Workload::Mixed;
            } else {
                std::fprintf(stderr, "kv_bench: unknown workload: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--keys" && has_value) {
            args->keys = std::atoi(argv[++i]);
        } else if (arg == "--value-size" && has_value) {
            args->value_size = std::atoi(argv[++i]);
        } else if (arg == "--threads" && has_value) {
            args->threads = std::atoi(argv[++i]);
        } else if (arg == "--dir" && has_value) {
            args->dir = argv[++i];
        } else {
            std::fprintf(stderr, "kv_bench: unrecognised argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (args->keys <= 0 || args->value_size < 0 || args->threads <= 0) {
        std::fprintf(stderr, "kv_bench: --keys, --value-size and --threads must be positive\n");
        return false;
    }
    return true;
}

// One engine behind the interface, plus the two things the interface does not
// carry: a way to force everything to disk, and the counters.
struct Harness {
    std::unique_ptr<kvstore::KVStore> store;
    kvstore::LsmStore* lsm = nullptr;  // Null for Bitcask.

    [[nodiscard]] kvstore::Status settle() const {
        if (lsm != nullptr) {
            if (const kvstore::Status flushed = lsm->flush(); !flushed.is_ok()) {
                return flushed;
            }
            return lsm->wait_for_background();
        }
        return {};
    }
};

Harness open_engine(const Args& args) {
    Harness harness;
    if (args.engine == Engine::Bitcask) {
        kvstore::BitcaskOptions options;
        options.sync_mode = kvstore::SyncMode::Never;
        auto opened = kvstore::Bitcask::open(args.dir, options);
        if (!opened.is_ok()) {
            std::fprintf(stderr, "kv_bench: %s\n", opened.status().to_string().c_str());
            std::exit(1);
        }
        harness.store = opened.take();
        return harness;
    }

    kvstore::LsmOptions options;
    // Never, for both engines: this measures the engine, not the device's fsync
    // latency, and an fsync per write would drown every other number.
    options.sync_mode = kvstore::SyncMode::Never;
    options.read_mode = args.read_mode;
    auto opened = kvstore::LsmStore::open(args.dir, options);
    if (!opened.is_ok()) {
        std::fprintf(stderr, "kv_bench: %s\n", opened.status().to_string().c_str());
        std::exit(1);
    }
    auto lsm = opened.take();
    harness.lsm = lsm.get();
    harness.store = std::move(lsm);
    return harness;
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

    bool temporary = false;
    if (args.dir.empty()) {
        args.dir = std::filesystem::temp_directory_path() /
                   ("kv_bench_" + std::to_string(std::chrono::steady_clock::now()
                                                     .time_since_epoch()
                                                     .count()));
        temporary = true;
    }
    std::error_code ec;
    std::filesystem::create_directories(args.dir, ec);

    const std::string value(static_cast<std::size_t>(args.value_size), 'x');
    const std::uint64_t user_bytes =
        static_cast<std::uint64_t>(args.keys) * (key_at(0).size() + value.size());

    Harness harness = open_engine(args);

    // --- Load ---------------------------------------------------------------
    //
    // Always sequential, whatever the workload: the read benchmarks need
    // something to read, and the write benchmarks time their own loop below.
    const auto load_start = std::chrono::steady_clock::now();
    for (int i = 0; i < args.keys; ++i) {
        const kvstore::Status status = harness.store->put(key_at(i), value);
        if (!status.is_ok()) {
            std::fprintf(stderr, "kv_bench: put failed: %s\n", status.to_string().c_str());
            return 1;
        }
    }
    if (const kvstore::Status settled = harness.settle(); !settled.is_ok()) {
        std::fprintf(stderr, "kv_bench: %s\n", settled.to_string().c_str());
        return 1;
    }
    const auto load_end = std::chrono::steady_clock::now();
    const double load_seconds =
        std::chrono::duration<double>(load_end - load_start).count();

    // Amplification is measured over the load, because that is the phase that
    // actually wrote and compacted anything.
    const std::uint64_t bytes_on_disk = directory_bytes(args.dir);
    std::uint64_t bytes_written = 0;
    std::uint64_t blocks_before = 0;
    std::uint64_t gets_before = 0;
    if (harness.lsm != nullptr) {
        bytes_written = harness.lsm->stats().bytes_written.load() +
                        harness.lsm->stats().compaction_bytes_written.load();
        blocks_before = harness.lsm->stats().block_reads.load();
        gets_before = harness.lsm->stats().gets.load();
    }

    // --- The measured phase -------------------------------------------------

    std::atomic<std::uint64_t> operations{0};
    std::vector<std::vector<double>> samples(static_cast<std::size_t>(args.threads));

    const auto run = [&](int thread_index) {
        std::mt19937 rng(static_cast<unsigned>(thread_index) * 7919u + 17u);
        std::uniform_int_distribution<int> pick(0, args.keys - 1);
        std::vector<double>& latencies = samples[static_cast<std::size_t>(thread_index)];
        latencies.reserve(static_cast<std::size_t>(args.keys));

        for (int n = 0; n < args.keys; ++n) {
            const auto start = std::chrono::steady_clock::now();
            switch (args.workload) {
                case Workload::SeqWrite:
                    (void)harness.store->put(key_at(n), value);
                    break;
                case Workload::RandWrite:
                    (void)harness.store->put(key_at(pick(rng)), value);
                    break;
                case Workload::RandRead: {
                    std::string out;
                    (void)harness.store->get(key_at(pick(rng)), &out);
                    break;
                }
                case Workload::Missing: {
                    std::string out;
                    // A suffix, so the key sorts *between* two real ones and
                    // falls inside every table's [min_key, max_key]. Without
                    // that the cheap range check answers first and the filter is
                    // never consulted -- which reports a beautiful read
                    // amplification of zero while measuring the wrong mechanism
                    // entirely. (The same trap caught a Step 2 test.)
                    (void)harness.store->get(key_at(pick(rng)) + "z", &out);
                    break;
                }
                case Workload::Mixed: {
                    if ((n % 4) == 0) {
                        (void)harness.store->put(key_at(pick(rng)), value);
                    } else {
                        std::string out;
                        (void)harness.store->get(key_at(pick(rng)), &out);
                    }
                    break;
                }
            }
            const auto end = std::chrono::steady_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
            ++operations;
        }
    };

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(args.threads));
    for (int t = 0; t < args.threads; ++t) {
        workers.emplace_back(run, t);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();

    // --- Report -------------------------------------------------------------

    std::vector<double> all;
    for (const std::vector<double>& thread_samples : samples) {
        all.insert(all.end(), thread_samples.begin(), thread_samples.end());
    }
    std::sort(all.begin(), all.end());

    const char* engine_name = args.engine == Engine::Bitcask ? "bitcask" : "lsm";
    const char* mode_name = args.read_mode == kvstore::ReadMode::Mmap ? "mmap" : "pread";
    static const char* kWorkloadNames[] = {"seqwrite", "randwrite", "randread", "missing",
                                           "mixed"};

    std::printf("engine        %s%s\n", engine_name,
                args.engine == Engine::Lsm ? (std::string(" (") + mode_name + ")").c_str() : "");
    std::printf("workload      %s\n", kWorkloadNames[static_cast<int>(args.workload)]);
    std::printf("keys          %d x %d-byte values, %d thread(s)\n", args.keys, args.value_size,
                args.threads);
    std::printf("\n");
    std::printf("load          %.2f s  (%.0f ops/s)\n", load_seconds,
                static_cast<double>(args.keys) / load_seconds);
    std::printf("throughput    %.0f ops/s  (%llu ops in %.2f s)\n",
                static_cast<double>(operations.load()) / seconds,
                static_cast<unsigned long long>(operations.load()), seconds);
    std::printf("latency       p50 %.1f us   p99 %.1f us   p999 %.1f us\n",
                percentile(all, 0.50), percentile(all, 0.99), percentile(all, 0.999));

    if (harness.lsm != nullptr) {
        const kvstore::LsmStats& stats = harness.lsm->stats();
        const std::uint64_t gets = stats.gets.load() - gets_before;
        const std::uint64_t blocks = stats.block_reads.load() - blocks_before;

        std::printf("\n");
        std::printf("write amp     %.2fx   (%llu bytes written / %llu bytes of data)\n",
                    user_bytes == 0 ? 0.0
                                    : static_cast<double>(bytes_written) /
                                          static_cast<double>(user_bytes),
                    static_cast<unsigned long long>(bytes_written),
                    static_cast<unsigned long long>(user_bytes));
        std::printf("read amp      %.3f    (%llu block reads / %llu gets)\n",
                    gets == 0 ? 0.0
                              : static_cast<double>(blocks) / static_cast<double>(gets),
                    static_cast<unsigned long long>(blocks),
                    static_cast<unsigned long long>(gets));
        std::printf("space amp     %.2fx   (%llu bytes on disk / %llu bytes of data)\n",
                    user_bytes == 0 ? 0.0
                                    : static_cast<double>(bytes_on_disk) /
                                          static_cast<double>(user_bytes),
                    static_cast<unsigned long long>(bytes_on_disk),
                    static_cast<unsigned long long>(user_bytes));
        std::printf("\n");
        std::printf("bloom         %llu checks, %llu rejected (%.1f%%)\n",
                    static_cast<unsigned long long>(stats.bloom_checks.load()),
                    static_cast<unsigned long long>(stats.bloom_rejects.load()),
                    stats.bloom_checks.load() == 0
                        ? 0.0
                        : 100.0 * static_cast<double>(stats.bloom_rejects.load()) /
                              static_cast<double>(stats.bloom_checks.load()));
        std::printf("compactions   %llu  (%llu bytes read, %llu written)\n",
                    static_cast<unsigned long long>(stats.compactions.load()),
                    static_cast<unsigned long long>(stats.compaction_bytes_read.load()),
                    static_cast<unsigned long long>(stats.compaction_bytes_written.load()));
        std::printf("levels        ");
        for (std::size_t level = 0; level < harness.lsm->level_count(); ++level) {
            std::printf("L%zu:%zu ", level, harness.lsm->tables_at(level));
        }
        std::printf("\n");
    } else {
        std::printf("\n");
        std::printf("space amp     %.2fx   (%llu bytes on disk / %llu bytes of data)\n",
                    user_bytes == 0 ? 0.0
                                    : static_cast<double>(bytes_on_disk) /
                                          static_cast<double>(user_bytes),
                    static_cast<unsigned long long>(bytes_on_disk),
                    static_cast<unsigned long long>(user_bytes));
        std::printf("(read and write amplification are LSM-only counters)\n");
    }

    // Release the engine before removing the directory: on Windows a file with
    // an open handle or a live mapping cannot be unlinked.
    harness.lsm = nullptr;
    harness.store.reset();
    if (temporary) {
        std::error_code remove_ec;
        std::filesystem::remove_all(args.dir, remove_ec);
    }
    return 0;
}
