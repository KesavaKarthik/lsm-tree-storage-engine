#include "kvstore/server.hpp"

#include <utility>

namespace kvstore {

Server::Server(KVStore& store, Listener listener, const ServerOptions& options)
    : store_(store),
      options_(options),
      listener_(std::move(listener)),
      port_(listener_.port()) {}

Result<std::unique_ptr<Server>> Server::start(KVStore& store, const ServerOptions& options) {
    auto listener = Listener::bind(options.host, options.port, options.backlog);
    if (!listener.is_ok()) {
        return listener.status();
    }

    // `new` rather than make_unique: the constructor is private, and making
    // make_unique a friend would let anyone construct one through it.
    std::unique_ptr<Server> server{new Server{store, listener.take(), options}};

    // Started only once the object is fully built. A thread launched from the
    // constructor would be running against a half-initialised `this`, and the
    // member it happened to touch first would decide whether that mattered.
    server->accept_thread_ = std::thread{[raw = server.get()] { raw->accept_loop(); }};

    return server;
}

Server::~Server() { stop(); }

std::size_t Server::active_connections() const {
    const std::lock_guard<std::mutex> guard{conns_mu_};
    return conns_.size();
}

void Server::accept_loop() {
    while (!stopping_.load(std::memory_order_relaxed)) {
        // Wait with a timeout rather than blocking in accept(). accept() cannot
        // be cancelled -- there is no portable way to interrupt a thread parked
        // in it -- so the loop turns one uninterruptible wait into a short one
        // it can come back from to re-check whether it should still be running.
        auto ready = listener_.wait_readable(options_.poll_timeout_ms);
        if (!ready.is_ok()) {
            break;  // The listener is closed or broken; there is nothing to accept from.
        }

        reap_finished();

        if (!*ready) {
            continue;  // Timed out. Around again, and stopping_ gets another look.
        }
        if (stopping_.load(std::memory_order_relaxed)) {
            break;
        }

        auto accepted = listener_.accept();
        if (!accepted.is_ok()) {
            // One failed accept is not a reason to stop serving everyone else --
            // the client may simply have hung up between the poll and the accept.
            continue;
        }

        accepted_.fetch_add(1, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> guard{conns_mu_};
        auto conn = std::make_unique<Connection>(accepted.take());
        Connection* raw = conn.get();
        conns_.push_back(std::move(conn));

        // Started last, so the Connection is fully in place -- and in the vector
        // -- before anything can be serving out of it.
        raw->thread = std::thread{[this, raw] { serve_connection(raw); }};
    }
}

void Server::reap_finished() {
    const std::lock_guard<std::mutex> guard{conns_mu_};

    for (auto it = conns_.begin(); it != conns_.end();) {
        if ((*it)->finished.load(std::memory_order_acquire)) {
            // Joining under the lock is safe: a finished thread's last act is to
            // raise the flag and return, and it takes no lock on the way out, so
            // there is nothing for this to wait on that could wait on this.
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::serve_connection(Connection* conn) {
    // One reader for the life of the connection. It has to outlive individual
    // requests because a single recv can deliver a message and a half, and the
    // half belongs to the request after this one.
    //
    // The policy is what makes this thread joinable. Without it the reader
    // blocks in recv() on an idle connection and nothing -- not shutdown(), not
    // closing the socket safely -- can portably get it out of there, so stop()
    // would wait on a thread that only returns when the client happens to leave.
    const protocol::ReadPolicy policy{options_.poll_timeout_ms, &stopping_};
    protocol::FrameReader reader{conn->sock, options_.max_frame, policy};
    std::vector<std::uint8_t> payload;

    while (!stopping_.load(std::memory_order_relaxed)) {
        auto incoming = reader.read_frame(&payload);

        if (!incoming.is_ok()) {
            // Either the framing broke -- in which case the position in the
            // stream is no longer known and nothing after it can be trusted --
            // or the socket did, which stop() causes deliberately. Both end the
            // connection, and neither is worth answering: a reply would have to
            // go out at an offset the peer no longer agrees with.
            break;
        }
        if (*incoming == protocol::FrameStatus::PeerClosed) {
            break;  // An ordinary hang-up.
        }
        if (*incoming == protocol::FrameStatus::Cancelled) {
            break;  // The server is shutting down; this connection goes with it.
        }

        const protocol::Response response = handle(payload);

        if (Status sent = protocol::write_frame(conn->sock, protocol::encode_response(response));
            !sent.is_ok()) {
            break;  // Client went away mid-answer. Nothing to report it to.
        }
    }

    // Last act. The socket is not closed here -- the Connection owns it, and the
    // accept loop closes it when it reaps this entry, which it will only do
    // after joining this thread.
    conn->finished.store(true, std::memory_order_release);
}

protocol::Response Server::handle(std::span<const std::uint8_t> payload) {
    auto request = protocol::decode_request(payload);
    if (!request.is_ok()) {
        // A malformed *message*, not a malformed stream. The frame boundaries
        // are still known, so this is answerable and the connection survives it.
        const Status status = request.status();
        return protocol::Response{status.code(), status.message()};
    }

    switch (request->op) {
        case protocol::Op::Get: {
            std::string value;
            const Status status = store_.get(request->key, &value);
            if (!status.is_ok()) {
                return protocol::Response{status.code(), status.message()};
            }
            return protocol::Response{StatusCode::Ok, std::move(value)};
        }
        case protocol::Op::Put: {
            // Note what is *not* checked here: whether the key is empty, whether
            // the value is too large, whether a delete of an absent key is an
            // error. Those are the KVStore contract's answers to give, and
            // second-guessing them here would be a second place they are defined.
            const Status status = store_.put(request->key, request->value);
            return protocol::Response{status.code(), status.message()};
        }
        case protocol::Op::Delete: {
            const Status status = store_.remove(request->key);
            return protocol::Response{status.code(), status.message()};
        }
    }

    return protocol::Response{StatusCode::InvalidArgument, "unhandled op"};
}

void Server::stop() noexcept {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;  // Idempotent: the destructor calls this after an explicit stop().
    }

    // A nudge, not the mechanism. What actually stops the connection threads is
    // stopping_, which each of them checks every poll interval -- see the
    // ReadPolicy handed to their FrameReader in serve_connection.
    //
    // This shutdown() only makes it prompt: on POSIX it ends a pending recv
    // immediately instead of after up to poll_timeout_ms. On Windows it does
    // nothing useful at all, which is exactly why correctness cannot rest on it.
    // close() instead of shutdown() would be worse than useless -- closing a
    // handle another thread is inside a syscall on is a race, and once the
    // number is recycled the next connection inherits the consequences.
    {
        const std::lock_guard<std::mutex> guard{conns_mu_};
        for (const auto& conn : conns_) {
            (void)conn->sock.shutdown();
        }
    }

    // The accept loop notices stopping_ within one poll timeout.
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Past this point the accept thread is gone, so nothing can add to conns_ or
    // reap from it, and the lock has no work left to do. Taking it anyway would
    // be free; not taking it would be a puzzle for the next reader. Taken.
    {
        const std::lock_guard<std::mutex> guard{conns_mu_};
        for (const auto& conn : conns_) {
            if (conn->thread.joinable()) {
                conn->thread.join();
            }
        }
        // Destroying the Connections is what finally closes their sockets --
        // after every thread that could have been using one has been joined.
        conns_.clear();
    }

    (void)listener_.close();
}

}  // namespace kvstore
