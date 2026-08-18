#include "kvstore/socket.hpp"

#include <string>
#include <utility>

#include "platform_socket.hpp"

namespace kvstore {

// --- Socket -----------------------------------------------------------------

Result<Socket> Socket::connect(const std::string& host, std::uint16_t port) {
    const socket_t sock = platform::connect_to(host, port);
    if (!is_valid_socket(sock)) {
        return Status::io_error("connect to " + host + ":" + std::to_string(port) + ": " +
                                platform::last_socket_error());
    }
    return Socket{sock};
}

Socket::~Socket() { close_if_open(); }

Socket::Socket(Socket&& other) noexcept : sock_(std::exchange(other.sock_, kInvalidSocket)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close_if_open();  // We may already own a different socket.
        sock_ = std::exchange(other.sock_, kInvalidSocket);
    }
    return *this;
}

void Socket::close_if_open() noexcept {
    if (is_valid_socket(sock_)) {
        // Nothing useful to do if this fails, and a destructor must not throw.
        (void)platform::close_socket(sock_);
        sock_ = kInvalidSocket;
    }
}

Status Socket::check_open(const char* what) const {
    if (!is_valid_socket(sock_)) {
        return Status::io_error(std::string{what} + " on a closed socket");
    }
    return Status::ok();
}

Status Socket::close() {
    if (!is_valid_socket(sock_)) {
        return Status::ok();  // Idempotent: closing twice is not an error.
    }
    const int rc = platform::close_socket(sock_);
    // Clear the handle either way. A failed close does not leave the socket
    // usable, and retrying would be a double close -- which, once the number is
    // recycled, closes somebody else's connection.
    sock_ = kInvalidSocket;
    if (rc != 0) {
        return Status::io_error("close socket: " + platform::last_socket_error());
    }
    return Status::ok();
}

Result<bool> Socket::wait_readable(int timeout_ms) {
    KVSTORE_RETURN_IF_ERROR(check_open("poll"));

    const int rc = platform::poll_readable(sock_, timeout_ms);
    if (rc < 0) {
        return Status::io_error("poll: " + platform::last_socket_error());
    }
    return rc > 0;
}

Status Socket::shutdown() {
    KVSTORE_RETURN_IF_ERROR(check_open("shutdown"));
    if (platform::shutdown_both(sock_) != 0) {
        // Not an error worth propagating in the usual case: the peer may already
        // have gone, which is exactly when a shutdown is least necessary. The
        // caller (Server::stop) ignores it deliberately.
        return Status::io_error("shutdown: " + platform::last_socket_error());
    }
    return Status::ok();
}

Status Socket::send_all(std::span<const std::uint8_t> bytes) {
    KVSTORE_RETURN_IF_ERROR(check_open("send"));

    // send() takes what fits in the kernel's send buffer and returns, so a
    // single call writing the whole message is not something to rely on. Exactly
    // the short-write loop in LogFile::append, for the same reason: treating a
    // partial transfer as success is how a peer ends up with half a message and
    // no way to know it.
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::int64_t n =
            platform::send_some(sock_, bytes.data() + sent, bytes.size() - sent);
        if (n < 0) {
            return Status::io_error("send: " + platform::last_socket_error());
        }
        if (n == 0) {
            // A blocking socket should not do this. If it ever does, looping
            // forever on no progress is worse than saying so.
            return Status::io_error("send made no progress");
        }
        sent += static_cast<std::size_t>(n);
    }
    return Status::ok();
}

Result<std::size_t> Socket::recv_some(std::span<std::uint8_t> buf) {
    KVSTORE_RETURN_IF_ERROR(check_open("recv"));

    if (buf.empty()) {
        // recv() with a zero-length buffer returns 0, which this API reserves
        // for "the peer closed". Answer without asking the kernel.
        return std::size_t{0};
    }

    const std::int64_t n = platform::recv_some(sock_, buf.data(), buf.size());
    if (n < 0) {
        return Status::io_error("recv: " + platform::last_socket_error());
    }
    return static_cast<std::size_t>(n);
}

// --- Listener ---------------------------------------------------------------

Result<Listener> Listener::bind(const std::string& host, std::uint16_t port, int backlog) {
    const socket_t sock = platform::listen_on(host, port, backlog);
    if (!is_valid_socket(sock)) {
        return Status::io_error("listen on " + host + ":" + std::to_string(port) + ": " +
                                platform::last_socket_error());
    }

    // Resolve the port now rather than on demand. With port 0 the kernel chose
    // one at bind time, and this is the only way to learn which -- asking later
    // would mean the handle has to still be open to answer a question whose
    // answer stopped changing the moment bind() returned.
    const int bound = platform::local_port(sock);
    if (bound < 0) {
        const std::string err = platform::last_socket_error();
        (void)platform::close_socket(sock);  // Don't leak the socket on the error path.
        return Status::io_error("getsockname: " + err);
    }

    return Listener{sock, static_cast<std::uint16_t>(bound)};
}

Listener::~Listener() { close_if_open(); }

Listener::Listener(Listener&& other) noexcept
    : sock_(std::exchange(other.sock_, kInvalidSocket)),
      port_(std::exchange(other.port_, std::uint16_t{0})) {}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        close_if_open();
        sock_ = std::exchange(other.sock_, kInvalidSocket);
        port_ = std::exchange(other.port_, std::uint16_t{0});
    }
    return *this;
}

void Listener::close_if_open() noexcept {
    if (is_valid_socket(sock_)) {
        (void)platform::close_socket(sock_);
        sock_ = kInvalidSocket;
    }
}

Status Listener::check_open(const char* what) const {
    if (!is_valid_socket(sock_)) {
        return Status::io_error(std::string{what} + " on a closed listener");
    }
    return Status::ok();
}

Status Listener::close() {
    if (!is_valid_socket(sock_)) {
        return Status::ok();
    }
    const int rc = platform::close_socket(sock_);
    sock_ = kInvalidSocket;
    if (rc != 0) {
        return Status::io_error("close listener: " + platform::last_socket_error());
    }
    return Status::ok();
}

Result<Socket> Listener::accept() {
    KVSTORE_RETURN_IF_ERROR(check_open("accept"));

    const socket_t sock = platform::accept_one(sock_);
    if (!is_valid_socket(sock)) {
        return Status::io_error("accept: " + platform::last_socket_error());
    }
    return Socket{sock};
}

Result<bool> Listener::wait_readable(int timeout_ms) {
    KVSTORE_RETURN_IF_ERROR(check_open("poll"));

    const int rc = platform::poll_readable(sock_, timeout_ms);
    if (rc < 0) {
        return Status::io_error("poll: " + platform::last_socket_error());
    }
    return rc > 0;
}

}  // namespace kvstore
