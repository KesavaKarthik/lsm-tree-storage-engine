#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kvstore/kvstore.hpp"
#include "kvstore/protocol.hpp"
#include "kvstore/result.hpp"
#include "kvstore/socket.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

struct ServerOptions {
    // Loopback by default. A key-value store with no authentication, no
    // authorisation and no transport security should not be reachable from
    // another machine because somebody forgot to say so -- exposing it has to be
    // a decision, which is what changing this is.
    std::string host = "127.0.0.1";

    // 0 means "any free port", which is what the tests use. port() reports what
    // was actually bound.
    std::uint16_t port = 0;

    int backlog = 128;

    // How long the accept loop waits before re-checking whether it should stop.
    // The only thing this trades is how long stop() takes in the worst case.
    int poll_timeout_ms = 100;

    std::uint32_t max_frame = protocol::kMaxFrameSize;
};

// A TCP server that speaks the framed protocol and serves it out of a KVStore.
//
// **It holds a KVStore&, not a Bitcask&, and that is the point.** Everything it
// does is decode a request, call one of three virtuals, and encode the answer;
// it has no idea whether there is a disk behind them. The tests exercise this
// directly by running the same round trips against a MemoryStore, with no
// storage engine in the process at all.
//
// **Thread per connection.** One thread blocks in accept, and every accepted
// connection gets a thread that blocks in recv until the client goes away. It is
// the simplest model that is correct, and its limits are honest ones: a thread
// and its stack per connection, so it stops being reasonable in the low
// thousands, which is the point at which the answer is an event loop.
//
// **This class does no locking of the store.** It calls whatever KVStore it was
// given, from many threads at once. Handing it an engine that is not thread-safe
// is a bug in the caller; handing it a LockedStore is the fix. That separation
// is deliberate -- see kvstore/locked_store.hpp.
class Server {
public:
    // Binds, starts listening, and starts accepting. The listening socket is
    // bound before this returns, so port() is valid immediately and a test can
    // connect without waiting for anything.
    [[nodiscard]] static Result<std::unique_ptr<Server>> start(KVStore& store,
                                                               const ServerOptions& options = {});

    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // The port actually bound. Still valid after stop().
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Stops accepting, breaks every open connection, and joins every thread.
    // Idempotent, and called by the destructor -- so a Server that goes out of
    // scope leaves no thread and no socket behind, which is what lets a test
    // start and stop dozens of them without leaking.
    void stop() noexcept;

    // Diagnostics, for tests. Not part of any contract.
    [[nodiscard]] std::uint64_t connections_accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t active_connections() const;

private:
    // One connection: its socket, the thread serving it, and a flag the thread
    // raises on its way out.
    //
    // The socket lives *here* rather than in the serving thread, and that is
    // what makes stop() safe. stop() has to reach into a connection another
    // thread is blocked inside a syscall on; if the thread owned the socket, the
    // handle could be closed and its number recycled between stop() reading it
    // and stop() using it -- and the shutdown would then land on whatever
    // connection had opened next. Because the Connection owns the socket and is
    // only destroyed under conns_mu_ after its thread has been joined, there is
    // no such window.
    struct Connection {
        explicit Connection(Socket socket) noexcept : sock(std::move(socket)) {}

        Socket sock;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    Server(KVStore& store, Listener listener, const ServerOptions& options);

    void accept_loop();
    void serve_connection(Connection* conn);
    void reap_finished();

    // Decodes one request, calls the store, and builds the answer. Never throws
    // and never fails: a request the store rejects is a response, not an error.
    [[nodiscard]] protocol::Response handle(std::span<const std::uint8_t> payload);

    KVStore& store_;
    ServerOptions options_;
    Listener listener_;
    std::uint16_t port_ = 0;

    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;

    mutable std::mutex conns_mu_;
    std::vector<std::unique_ptr<Connection>> conns_;

    std::atomic<std::uint64_t> accepted_{0};
};

}  // namespace kvstore
