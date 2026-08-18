#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "kvstore/result.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// The OS handle for a socket.
//
// Defined in this public header rather than down in the platform shim because
// Server's connection registry names the type too, and a second typedef that has
// to agree with the first is a bug waiting for a platform to disagree on.
//
// Windows: a Winsock SOCKET is a UINT_PTR whose failure value is INVALID_SOCKET
// == ~0, not -1. Putting one in an `int` truncates on 64-bit, and testing it
// with `< 0` is wrong for every socket whose top bit happens to be set. POSIX: an
// ordinary file descriptor, where -1 is the failure value as usual.
#ifdef _WIN32
using socket_t = std::uintptr_t;
inline constexpr socket_t kInvalidSocket = ~static_cast<socket_t>(0);
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = static_cast<socket_t>(-1);
#endif

[[nodiscard]] inline bool is_valid_socket(socket_t sock) noexcept {
    return sock != kInvalidSocket;
}

// Owns one connected socket, with exactly the discipline LogFile uses for a file
// descriptor: move-only, closed on every exit path by the destructor, an
// idempotent close() for releasing it early, and a check on every entry point so
// a closed socket returns IOError rather than handing an invalid handle to a
// syscall.
//
// The reason is the same one, restated: two objects holding one handle means the
// first destructor closes something the second still uses, and handle numbers get
// recycled -- so the second object's writes end up in whatever connection opened
// next. On a socket that is not a corrupted file, it is one client being sent
// another client's data.
class Socket {
public:
    [[nodiscard]] static Result<Socket> connect(const std::string& host, std::uint16_t port);

    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Writes every byte or fails, looping on short sends the way LogFile::append
    // loops on short writes. The kernel's send buffer fills and send() takes what
    // fits, so a single call writing the whole message is not something to rely on.
    [[nodiscard]] Status send_all(std::span<const std::uint8_t> bytes);

    // Reads whatever has already arrived, up to buf.size(). Returns the byte
    // count, or 0 when the peer has closed its end.
    //
    // **This does not fill `buf`, and callers must never assume it does.** It
    // returns what the kernel happens to be holding at this instant: one byte of
    // a message the sender wrote in a single call, or two whole messages at once.
    // Reassembling messages from this is the frame reader's job, not this call's.
    [[nodiscard]] Result<std::size_t> recv_some(std::span<std::uint8_t> buf);

    // Waits up to `timeout_ms` for the socket to have something to read. True
    // means a following recv_some() will not block; false is a timeout, which is
    // not an error.
    //
    // **This is how a read loop stays interruptible**, and it is not optional.
    // See the warning on shutdown() below.
    [[nodiscard]] Result<bool> wait_readable(int timeout_ms);

    // Closes both directions without releasing the handle.
    //
    // **This is not a reliable way to wake a thread already blocked in recv(),
    // and believing that it is cost this project an afternoon.** On POSIX it
    // works: the read side ends and the pending recv returns 0. On Winsock it
    // does not -- shutdown() returns success, the blocked recv keeps waiting,
    // and it comes back roughly two minutes later when TCP gives up. A server
    // whose shutdown depends on this appears to work on Linux and hangs for two
    // minutes per teardown on Windows.
    //
    // What it is good for: a connection that is *about* to be read from returns
    // immediately rather than blocking, and on POSIX a blocked reader is woken
    // at once instead of within a poll interval. Correctness has to come from
    // the reader never blocking indefinitely -- see wait_readable().
    [[nodiscard]] Status shutdown();

    // Releases the handle early. Idempotent; every other operation returns
    // IOError afterwards.
    [[nodiscard]] Status close();

    [[nodiscard]] bool is_open() const noexcept { return is_valid_socket(sock_); }

    // The raw handle, for Server's connection registry. Not an ownership
    // transfer: the Socket still closes it.
    [[nodiscard]] socket_t native() const noexcept { return sock_; }

private:
    friend class Listener;  // accept() constructs a Socket from a fresh handle.

    explicit Socket(socket_t sock) noexcept : sock_(sock) {}

    void close_if_open() noexcept;
    [[nodiscard]] Status check_open(const char* what) const;

    socket_t sock_ = kInvalidSocket;  // Invalid means "moved from" or "closed".
};

// Owns a listening socket. Same rules as Socket.
class Listener {
public:
    // Binds and starts listening. `port` may be 0, meaning "any free port" --
    // the kernel picks one and port() reads it back, which is what lets a test
    // run a server without gambling on a port number some other process wants.
    [[nodiscard]] static Result<Listener> bind(const std::string& host, std::uint16_t port,
                                               int backlog = 128);

    ~Listener();

    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // The port actually bound, resolved via getsockname at bind time.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Blocks until a client connects.
    [[nodiscard]] Result<Socket> accept();

    // Waits up to `timeout_ms` for a connection to be pending. True means a
    // following accept() will not block.
    //
    // accept() on its own blocks forever and cannot be cancelled, so an accept
    // loop that wants to notice a stop request has to wait with a timeout first
    // and come back around.
    [[nodiscard]] Result<bool> wait_readable(int timeout_ms);

    [[nodiscard]] Status close();

    [[nodiscard]] bool is_open() const noexcept { return is_valid_socket(sock_); }

    [[nodiscard]] socket_t native() const noexcept { return sock_; }

private:
    Listener(socket_t sock, std::uint16_t port) noexcept : sock_(sock), port_(port) {}

    void close_if_open() noexcept;
    [[nodiscard]] Status check_open(const char* what) const;

    socket_t sock_ = kInvalidSocket;
    std::uint16_t port_ = 0;
};

}  // namespace kvstore
