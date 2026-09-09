// Environment capture and summary statistics for the benchmark suite.
//
// A throughput number without the machine it came from is not a measurement,
// it is an anecdote: the same binary on the same code answers differently on a
// 9p mount, under a different kernel, or with half the RAM. Everything here
// exists so that every CSV row carries enough of its own context to be compared
// against a row taken a month later, and so that a run on the wrong filesystem
// stops rather than quietly producing numbers about the wrong thing.
//
// Header-only and deliberately free of GoogleTest, so a benchmark target can
// use it without linking a test framework.

#ifndef KVSTORE_BENCH_ENV_HPP
#define KVSTORE_BENCH_ENV_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace kvbench {

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

// Nearest-rank on an already-sorted range. No interpolation: with hundreds of
// thousands of samples the difference from a linear-interpolated percentile is
// far below the noise floor, and the reported value is always a sample that was
// actually observed rather than one synthesised between two.
inline double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double scaled = p * static_cast<double>(sorted.size() - 1);
    std::size_t index = static_cast<std::size_t>(scaled);
    if (index >= sorted.size()) {
        index = sorted.size() - 1;
    }
    return sorted[index];
}

// Copies deliberately: the caller's run order is worth keeping for the raw rows.
inline double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[mid];
    }
    return (values[mid - 1] + values[mid]) / 2.0;
}

// What one steady_clock::now() pair costs, in nanoseconds.
//
// Every measured operation is bracketed by two clock reads, so this lands
// directly in each latency sample. At roughly 25ns against a 1-2us operation it
// is about 2% -- small, but the honest thing is to report it as a column rather
// than to assert it is negligible and hope.
inline double clock_overhead_ns() {
    constexpr int kIterations = 200000;

    // volatile, and not inline asm: without a use the loop body is dead code
    // and vanishes under -O2, making the answer zero. A volatile store is the
    // portable way to say "this result is observable" -- the GCC-only
    // asm-volatile trick would not compile under MSVC, which this repo builds.
    volatile std::int64_t sink = 0;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        sink = std::chrono::steady_clock::now().time_since_epoch().count();
    }
    const auto stop = std::chrono::steady_clock::now();
    (void)sink;
    const double total_ns =
        std::chrono::duration<double, std::nano>(stop - start).count();
    return total_ns / kIterations;
}

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------

// Linux filesystem magic numbers. ext4 is the one we want; the others are named
// because they are the ones actually reachable on this machine and a message
// that says "9p" is worth more than one that says "0x1021997".
inline constexpr std::uint64_t kExt4Magic = 0xEF53;
inline constexpr std::uint64_t kV9fsMagic = 0x01021997;  // WSL /mnt/c interop.
inline constexpr std::uint64_t kTmpfsMagic = 0x01021994;
inline constexpr std::uint64_t kOverlayfsMagic = 0x794C7630;
inline constexpr std::uint64_t kBtrfsMagic = 0x9123683E;
inline constexpr std::uint64_t kXfsMagic = 0x58465342;

inline std::string filesystem_name(std::uint64_t magic) {
    switch (magic) {
        case kExt4Magic:
            return "ext4";
        case kV9fsMagic:
            return "9p";
        case kTmpfsMagic:
            return "tmpfs";
        case kOverlayfsMagic:
            return "overlayfs";
        case kBtrfsMagic:
            return "btrfs";
        case kXfsMagic:
            return "xfs";
        default:
            return "unknown";
    }
}

// 0 where the platform cannot say.
inline std::uint64_t filesystem_magic(const std::filesystem::path& path) {
#if defined(__linux__)
    struct statfs buf {};
    if (::statfs(path.c_str(), &buf) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(buf.f_type);
#else
    (void)path;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

struct Env {
    std::string hostname = "unknown";
    std::string kernel = "unknown";
    std::string cpu_model = "unknown";
    unsigned cores = 0;   // Physical cores per socket x sockets.
    unsigned threads = 0; // Logical CPUs, i.e. what nproc reports.
    std::uint64_t ram_bytes = 0;
    std::string fs_type = "unknown";
    std::uint64_t fs_magic = 0;
};

namespace detail {

// First value after the colon on the first line whose key matches, trimmed.
inline std::string proc_field(const char* file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, colon);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        if (name != key) {
            continue;
        }
        std::string value = line.substr(colon + 1);
        const std::size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return {};
        }
        return value.substr(first);
    }
    return {};
}

}  // namespace detail

inline Env capture_env(const std::filesystem::path& data_dir) {
    Env env;

#if defined(__linux__)
    utsname uts{};
    if (::uname(&uts) == 0) {
        env.kernel = uts.release;
        env.hostname = uts.nodename;
    }

    const std::string model = detail::proc_field("/proc/cpuinfo", "model name");
    if (!model.empty()) {
        env.cpu_model = model;
    }

    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        env.threads = static_cast<unsigned>(online);
    }

    // "cpu cores" is per-socket, so multiply by the socket count. Cheaper than
    // parsing lscpu, and correct on the single-socket machines this runs on.
    const std::string cores = detail::proc_field("/proc/cpuinfo", "cpu cores");
    if (!cores.empty()) {
        env.cores = static_cast<unsigned>(std::strtoul(cores.c_str(), nullptr, 10));
    }

    // MemTotal is in kB.
    const std::string mem = detail::proc_field("/proc/meminfo", "MemTotal");
    if (!mem.empty()) {
        env.ram_bytes = std::strtoull(mem.c_str(), nullptr, 10) * 1024ull;
    }
#endif

    env.fs_magic = filesystem_magic(data_dir);
    env.fs_type = filesystem_name(env.fs_magic);
    return env;
}

// Refuses to continue unless the data directory is ext4.
//
// This is the check that stops a whole afternoon of numbers being wrong. A 9p
// mount is roughly an order of magnitude slower per fsync and has different
// durability semantics, so a benchmark run there measures the Windows interop
// layer rather than the engine -- and it does so without any symptom other than
// numbers that look plausible.
inline void assert_ext4(const std::filesystem::path& dir, bool allow_non_ext4) {
    const std::uint64_t magic = filesystem_magic(dir);

#if !defined(__linux__)
    (void)magic;
    if (!allow_non_ext4) {
        std::fprintf(stderr,
                     "kv-benchsuite: filesystem type cannot be determined on this platform.\n"
                     "  Benchmarks are only meaningful on native ext4. Re-run under Linux,\n"
                     "  or pass --allow-non-ext4 to proceed anyway.\n");
        std::exit(2);
    }
    return;
#else
    if (magic == kExt4Magic) {
        return;
    }
    if (allow_non_ext4) {
        std::fprintf(stderr,
                     "kv-benchsuite: WARNING: %s is %s (0x%llx), not ext4. "
                     "Numbers from this run are not comparable.\n",
                     dir.string().c_str(), filesystem_name(magic).c_str(),
                     static_cast<unsigned long long>(magic));
        return;
    }
    std::fprintf(stderr,
                 "kv-benchsuite: refusing to run: %s is %s (0x%llx), not ext4.\n",
                 dir.string().c_str(), filesystem_name(magic).c_str(),
                 static_cast<unsigned long long>(magic));
    if (magic == kV9fsMagic) {
        std::fprintf(stderr,
                     "  That is a Windows drive mounted into WSL. Every fsync crosses the\n"
                     "  interop layer, so this would measure 9p, not the engine.\n"
                     "  Use a path under your Linux home instead, e.g. ~/kvbench/data.\n");
    }
    std::fprintf(stderr, "  Pass --allow-non-ext4 to override.\n");
    std::exit(2);
#endif
}

}  // namespace kvbench

#endif  // KVSTORE_BENCH_ENV_HPP
