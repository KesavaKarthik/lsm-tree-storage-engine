// kvstore_server -- serves a Bitcask database over TCP.
//
// This file is the only place where the three pieces meet: the storage engine,
// the lock that makes it safe to share, and the server that knows neither of
// them by name. Everything below main() is argument handling; the wiring is
// eight lines and is the point of the phase.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "kvstore/bitcask.hpp"
#include "kvstore/locked_store.hpp"
#include "kvstore/server.hpp"
#include "kvstore/status.hpp"

namespace {

// Written by the signal handler, read by main. sig_atomic_t and nothing else:
// a handler may only touch a volatile sig_atomic_t and call async-signal-safe
// functions, which rules out almost everything, printing included.
volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void on_interrupt(int) { g_interrupted = 1; }

struct Args {
    std::filesystem::path dir = "kvstore-data";
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;
    kvstore::SyncMode sync_mode = kvstore::SyncMode::Always;
    bool help = false;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: kvstore_server [options]\n"
                 "\n"
                 "  --dir PATH     database directory (default: kvstore-data)\n"
                 "  --host ADDR    address to bind (default: 127.0.0.1)\n"
                 "  --port N       port to bind, 0 for any (default: 7379)\n"
                 "  --no-sync      do not fsync every write -- faster, and a\n"
                 "                 power cut loses whatever was still in the\n"
                 "                 page cache\n"
                 "  --help\n");
}

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

    auto opened = kvstore::Bitcask::open(args.dir, kvstore::BitcaskOptions{args.sync_mode});
    if (!opened.is_ok()) {
        std::fprintf(stderr, "kvstore_server: cannot open %s: %s\n", args.dir.string().c_str(),
                     opened.status().to_string().c_str());
        return 1;
    }
    std::unique_ptr<kvstore::Bitcask> engine = opened.take();

    // The wiring. The engine is single-threaded, so it goes behind a lock; the
    // server takes the lock's KVStore face and never learns what is behind it.
    kvstore::LockedStore locked{*engine};

    kvstore::ServerOptions options;
    options.host = args.host;
    options.port = args.port;

    auto started = kvstore::Server::start(locked, options);
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

    std::fprintf(stderr, "kvstore_server: %s, %zu keys, listening on %s:%u (Ctrl-C to stop)\n",
                 args.dir.string().c_str(), engine->key_count(), args.host.c_str(),
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
    // to touch the engine directly: sync() is not on the KVStore interface, so
    // it is not behind LockedStore, and calling it while a connection thread was
    // mid-put would be exactly the unsynchronised access the lock exists to
    // prevent.
    server->stop();

    // ~Bitcask does not fsync -- it closes descriptors and no more. Under
    // SyncMode::Never every acknowledged write so far may still be in the page
    // cache, and this is what puts it on the device.
    if (const kvstore::Status synced = engine->sync(); !synced.is_ok()) {
        std::fprintf(stderr, "kvstore_server: final sync failed: %s\n",
                     synced.to_string().c_str());
        return 1;
    }

    return 0;
}
