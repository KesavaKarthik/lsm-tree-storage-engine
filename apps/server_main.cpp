// kvstore_server -- serves a kvstore database over TCP.
//
// This file is the only place where the three pieces meet: the storage engine,
// the lock that makes it safe to share, and the server that knows neither of
// them by name. Everything below main() is argument handling; the wiring is
// eight lines and is the point of the phase.
//
// Since Phase 4 there are two engines to choose between, and the fact that
// choosing is a one-line difference here -- with nothing in server.cpp or
// protocol.cpp aware that a second one exists -- is the KVStore interface
// earning its keep.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "kvstore/bitcask.hpp"
#include "kvstore/locked_store.hpp"
#include "kvstore/lsm.hpp"
#include "kvstore/server.hpp"
#include "kvstore/status.hpp"

namespace {

// Written by the signal handler, read by main. sig_atomic_t and nothing else:
// a handler may only touch a volatile sig_atomic_t and call async-signal-safe
// functions, which rules out almost everything, printing included.
volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void on_interrupt(int) { g_interrupted = 1; }

enum class Engine { Bitcask, Lsm };

struct Args {
    std::filesystem::path dir = "kvstore-data";
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;
    kvstore::SyncMode sync_mode = kvstore::SyncMode::Always;
    Engine engine = Engine::Bitcask;
    bool help = false;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: kvstore_server [options]\n"
                 "\n"
                 "  --dir PATH     database directory (default: kvstore-data)\n"
                 "  --engine NAME  bitcask or lsm (default: bitcask). The two\n"
                 "                 use incompatible on-disk layouts, so a\n"
                 "                 directory belongs to whichever wrote it\n"
                 "  --host ADDR    address to bind (default: 127.0.0.1)\n"
                 "  --port N       port to bind, 0 for any (default: 7379)\n"
                 "  --no-sync      do not fsync every write -- faster, and a\n"
                 "                 power cut loses whatever was still in the\n"
                 "                 page cache\n"
                 "  --help\n");
}

// Both engines have a sync() and something worth printing about what they hold,
// and neither is on the KVStore interface -- deliberately, since neither would
// mean anything for a MemoryStore. So main() keeps the concrete operations
// alongside the interface pointer, which is tidier than a nullable pointer per
// engine and an `if` at each of the three use sites.
struct OpenedEngine {
    std::unique_ptr<kvstore::KVStore> store;
    std::function<std::string()> describe;
    std::function<kvstore::Status()> shutdown;
};

// Returns false on a malformed argument, having said why.
bool parse_args(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            args->help = true;
            return true;
        }
        if (arg == "--no-sync") {
            args->sync_mode = kvstore::SyncMode::Never;
        } else if (arg == "--dir" && has_value) {
            args->dir = argv[++i];
        } else if (arg == "--engine" && has_value) {
            const std::string name = argv[++i];
            if (name == "bitcask") {
                args->engine = Engine::Bitcask;
            } else if (name == "lsm") {
                args->engine = Engine::Lsm;
            } else {
                std::fprintf(stderr, "kvstore_server: unknown engine: %s\n", name.c_str());
                return false;
            }
        } else if (arg == "--host" && has_value) {
            args->host = argv[++i];
        } else if (arg == "--port" && has_value) {
            const long value = std::strtol(argv[++i], nullptr, 10);
            if (value < 0 || value > 65535) {
                std::fprintf(stderr, "kvstore_server: port out of range: %ld\n", value);
                return false;
            }
            args->port = static_cast<std::uint16_t>(value);
        } else {
            std::fprintf(stderr, "kvstore_server: unrecognised argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
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

    OpenedEngine engine;
    if (args.engine == Engine::Bitcask) {
        auto opened = kvstore::Bitcask::open(args.dir, kvstore::BitcaskOptions{args.sync_mode});
        if (!opened.is_ok()) {
            std::fprintf(stderr, "kvstore_server: cannot open %s: %s\n", args.dir.string().c_str(),
                         opened.status().to_string().c_str());
            return 1;
        }
        auto* store = opened.take().release();
        engine.store.reset(store);
        engine.describe = [store] { return std::to_string(store->key_count()) + " keys"; };
        // ~Bitcask does not fsync -- it closes descriptors and no more. Under
        // SyncMode::Never every acknowledged write so far may still be in the
        // page cache, and this is what puts it on the device.
        engine.shutdown = [store] { return store->sync(); };
    } else {
        kvstore::LsmOptions options;
        options.sync_mode = args.sync_mode;
        auto opened = kvstore::LsmStore::open(args.dir, options);
        if (!opened.is_ok()) {
            std::fprintf(stderr, "kvstore_server: cannot open %s: %s\n", args.dir.string().c_str(),
                         opened.status().to_string().c_str());
            return 1;
        }
        auto* store = opened.take().release();
        engine.store.reset(store);
        engine.describe = [store] {
            return std::to_string(store->table_count()) + " tables, " +
                   std::to_string(store->memtable_entries()) + " keys in the memtable";
        };
        // Flushing on the way out is hygiene rather than durability -- the WAL
        // already holds everything, and recovery would replay it. It just means
        // the next start has nothing to do.
        engine.shutdown = [store] {
            if (const kvstore::Status flushed = store->flush(); !flushed.is_ok()) {
                return flushed;
            }
            return store->sync();
        };
    }

    // The wiring, and the one place the two engines are no longer alike.
    //
    // Bitcask is single-threaded, so it goes behind a lock and the server ends
    // up exactly as concurrent as the engine -- one request at a time, cores
    // idle. That was the honest trade in Phase 3 and it is still honest now.
    //
    // The LSM engine is served **directly**. It has its own reader/writer
    // concurrency: readers run in parallel with each other and with a writer,
    // and flushes and compactions happen on a background thread. Wrapping it in
    // LockedStore would serialise all of that away at the door, which is to say
    // it would throw away the entire point of the phase. Removing one wrapper is
    // the whole visible change, and it is only safe because every public method
    // of LsmStore is individually thread-safe.
    std::optional<kvstore::LockedStore> locked;
    kvstore::KVStore* served = engine.store.get();
    if (args.engine == Engine::Bitcask) {
        locked.emplace(*engine.store);
        served = &*locked;
    }

    kvstore::ServerOptions options;
    options.host = args.host;
    options.port = args.port;

    auto started = kvstore::Server::start(*served, options);
    if (!started.is_ok()) {
        std::fprintf(stderr, "kvstore_server: cannot listen: %s\n",
                     started.status().to_string().c_str());
        return 1;
    }
    std::unique_ptr<kvstore::Server> server = started.take();

    std::signal(SIGINT, on_interrupt);
#ifdef SIGTERM
    std::signal(SIGTERM, on_interrupt);
#endif

    std::fprintf(stderr, "kvstore_server: %s [%s], %s, listening on %s:%u (Ctrl-C to stop)\n",
                 args.dir.string().c_str(),
                 args.engine == Engine::Bitcask ? "bitcask" : "lsm",
                 engine.describe().c_str(), args.host.c_str(),
                 static_cast<unsigned>(server->port()));

    while (g_interrupted == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::fprintf(stderr, "\nkvstore_server: stopping\n");

    // Order matters, and this is the one place in the program where it is
    // subtle.
    //
    // stop() joins the accept thread and every connection thread, so when it
    // returns nothing is calling into the store any more. Only then is it safe
    // to touch the engine directly: sync() and flush() are not on the KVStore
    // interface, so on the Bitcask path they are not behind LockedStore, and
    // calling one while a connection thread was mid-put would be exactly the
    // unsynchronised access the lock exists to prevent. The LSM engine would
    // tolerate it, but the ordering is right for both and arguing the two cases
    // separately would be worse than obeying the stricter one.
    server->stop();

    if (const kvstore::Status stopped = engine.shutdown(); !stopped.is_ok()) {
        std::fprintf(stderr, "kvstore_server: final sync failed: %s\n",
                     stopped.to_string().c_str());
        return 1;
    }

    return 0;
}
