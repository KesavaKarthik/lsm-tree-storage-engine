#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "kvstore/socket.hpp"  // socket_t, kInvalidSocket -- see below.

namespace kvstore::platform {

// The socket half of the portability shim. Same job as platform_file.hpp -- one
// file that knows what Windows is -- but deliberately *not* the same model, and
// the differences are worth stating because the obvious move is to reuse the
// file shim and it does not work:
//
//   The handle    -- a POSIX socket is a file descriptor, so `int` and -1 carry
//                    over. A Winsock SOCKET is a UINT_PTR and its failure value
//                    is INVALID_SOCKET == ~0, not -1. Storing one in an `int`
//                    truncates on 64-bit, and testing it with `< 0` is wrong for
//                    every socket whose numeric value has the top bit set.
//                    Hence socket_t and kInvalidSocket rather than int and -1.
//   Closing       -- closesocket(), not _close(). A SOCKET is not a CRT
//                    descriptor and _close() on one is undefined.
//   Errors        -- Winsock never touches errno; it reports through
//                    WSAGetLastError(). platform_file's set_errno_from_win32
//                    maps *file* error codes and knows nothing about
//                    WSAECONNRESET, so sockets get their own last_socket_error()
//                    and this header makes no promise about errno at all.
//   Startup       -- Winsock needs a process-wide WSAStartup before the first
//                    socket call. POSIX needs nothing.
//
// What does carry over: every function returns -1 (or a negative count) on
// failure, so callers stay POSIX-shaped and read the same as the file paths.
//
// IPv4 only, on purpose. getaddrinfo and dual-stack listening are the general
// answer, and they cost a linked list to walk, a second address family to test
// and a v6-mapped-v4 policy to decide. This project's network layer exists to
// demonstrate framing and resource ownership; hard-coding AF_INET keeps the
// address handling to four readable lines. It is a real limitation, not an
// oversight.

// The handle type itself lives in kvstore/socket.hpp, because Server's public
// header names it too and one definition is the point. Aliased in rather than
// redefined here: two typedefs that must agree is exactly the arrangement that
// eventually stops agreeing.
using kvstore::kInvalidSocket;
using kvstore::socket_t;

[[nodiscard]] inline bool is_valid(socket_t sock) noexcept { return is_valid_socket(sock); }

// Initialises Winsock on first call; a no-op returning 0 on POSIX. Returns -1 if
// Winsock could not start. Idempotent and safe to call from several threads.
//
// Every entry point below that creates a socket calls this first, so callers
// never have to remember to. Cleanup happens at static destruction, which is
// after main() returns and therefore after any Server has joined its threads.
int socket_startup();

// A listening socket bound to `host`:`port`, or kInvalidSocket.
//
// `port` may be 0, meaning "any free port" -- the kernel picks one and
// local_port() reads it back. That is what lets a test run a server without
// choosing a port number that some other process on the machine might want.
[[nodiscard]] socket_t listen_on(const std::string& host, std::uint16_t port, int backlog);

// Blocks until a client arrives. Returns the connected socket or kInvalidSocket.
[[nodiscard]] socket_t accept_one(socket_t listener);

[[nodiscard]] socket_t connect_to(const std::string& host, std::uint16_t port);

int close_socket(socket_t sock);

// Up to `count` bytes. Returns the count read, 0 when the peer has closed its
// end, or -1. **Short reads are the normal case, not the exception** -- TCP is a
// byte stream and this returns whatever has arrived, which may be one byte of a
// message the sender wrote in one call. Callers must loop.
[[nodiscard]] std::int64_t recv_some(socket_t sock, void* buf, std::size_t count);

// Returns the count written or -1. Short writes happen when the kernel's send
// buffer fills; callers must loop, exactly as they do for pwrite.
[[nodiscard]] std::int64_t send_some(socket_t sock, const void* buf, std::size_t count);

// Closes both directions *without* releasing the handle.
//
// This is how a blocking recv() on another thread is broken. There is no
// portable way to interrupt a thread parked in recv(), but shutting the socket
// down makes that recv() return immediately, and the owning thread then unwinds
// and closes the socket itself. Closing the socket from the outside instead
// would be a use-after-free race on a handle another thread is inside a syscall
// on.
//
// The two platforms disagree about *how* that recv returns, and it is a trap:
// POSIX shuts the read side down as an orderly end-of-stream, so recv yields 0,
// whereas Winsock fails the call with WSAESHUTDOWN. A caller that only handles
// the 0 case spins forever on Windows.
int shutdown_both(socket_t sock);

// The port this socket is actually bound to, or -1. Only interesting after
// binding port 0.
[[nodiscard]] int local_port(socket_t sock);

// Waits up to `timeout_ms` for the socket to have something to read (for a
// listener, a pending connection). Returns 1 if readable, 0 on timeout, -1 on
// error.
//
// The accept loop needs this because accept() blocks with no timeout and cannot
// be cancelled: polling first turns an uninterruptible wait into a short one
// that comes back around to check whether the server is stopping.
[[nodiscard]] int poll_readable(socket_t sock, int timeout_ms);

// Message for the socket call that just failed. Reads WSAGetLastError() on
// Windows and errno on POSIX -- both are per-thread, so this is safe to call
// from a connection thread.
[[nodiscard]] std::string last_socket_error();

}  // namespace kvstore::platform
