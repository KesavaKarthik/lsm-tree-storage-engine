#include "platform_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "platform_file.hpp"  // last_error(), reused for errno on POSIX.

#ifdef _WIN32
// NOMINMAX before windows.h: it otherwise defines min and max as *macros*, and
// the preprocessor then eats `std::min(...)` and reports a syntax error on the
// `(` rather than anything resembling the actual problem.
#define NOMINMAX
// winsock2.h must come before windows.h. windows.h pulls in the Winsock 1.1
// declarations from winsock.h, and every symbol then collides with the 2.2 ones
// we actually want.
#include <winsock2.h>
#include <ws2tcpip.h>
// <windows.h> after the Winsock headers above.
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kvstore::platform {
namespace {

// A single recv/send call is capped rather than handed the caller's whole
// length. Windows's recv takes an `int`, so a size_t above INT_MAX would
// truncate silently -- and every caller already loops, because a short transfer
// is the normal case on a socket. Capping is therefore free.
constexpr std::size_t kMaxChunk = 1u << 20;

// ---- The parts that genuinely differ between the two platforms -------------
//
// Everything below this block is written once: socket/bind/listen/accept/
// connect/send/recv/shutdown/getsockname are the same BSD calls with the same
// names on Windows. Only the handle type, the error channel, the length types
// and the close call change, so only those are branched on. Duplicating the
// whole file would mean two copies of the bind-then-listen sequencing to keep
// in step.

#ifdef _WIN32

using len_t = int;
using addrlen_t = int;
constexpr int kShutdownBoth = SD_BOTH;

int last_error_code() noexcept { return ::WSAGetLastError(); }
void set_error_code(int code) noexcept { ::WSASetLastError(code); }

int close_native(socket_t sock) noexcept {
    return ::closesocket(static_cast<SOCKET>(sock));
}

// SO_REUSEADDR is deliberately *not* set on Windows.
//
// It does not mean there what it means on POSIX. On Linux it lets a new socket
// bind a port still in TIME_WAIT from a previous connection -- a courtesy that
// costs nothing. On Windows it lets a *second live socket* bind a port another
// process is already listening on, and connections are then handed to whichever
// of them the stack feels like. That is a way for an unrelated process to
// silently steal traffic, and Microsoft's own answer is not to set it.
void set_reuse_addr(socket_t) noexcept {}

std::string format_error(int code) {
    char* buf = nullptr;
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    if (n == 0 || buf == nullptr) {
        return "winsock error " + std::to_string(code);
    }
    std::string msg(buf, n);
    ::LocalFree(buf);
    // FormatMessage terminates its strings with ".\r\n", which reads badly in
    // the middle of one of our own messages.
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg;
}

struct WinsockGuard {
    WinsockGuard() {
        WSADATA data{};
        ok = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WinsockGuard() {
        if (ok) {
            ::WSACleanup();
        }
    }
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;

    bool ok = false;
};

#else  // POSIX

using len_t = std::size_t;
using addrlen_t = socklen_t;
constexpr int kShutdownBoth = SHUT_RDWR;

int last_error_code() noexcept { return errno; }
void set_error_code(int code) noexcept { errno = code; }

int close_native(socket_t sock) noexcept { return ::close(sock); }

// Without this, a test that starts a server, stops it, and starts another on
// the same port fails to bind: the listening socket's port sits in TIME_WAIT
// for up to two minutes after close. Safe here in a way it is not on Windows --
// see the comment on the other side of this #ifdef.
void set_reuse_addr(socket_t sock) noexcept {
    const int on = 1;
    (void)::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}

std::string format_error(int code) {
    const int saved = errno;
    errno = code;
    std::string msg = last_error();  // platform_file.cpp; thread-safe strerror.
    errno = saved;
    return msg;
}

#endif

// ---- Shared helpers --------------------------------------------------------

// Runs `f`, and if it fails, closes `sock` without losing the error that caused
// the failure. close() sets its own error code on the way out, so the reason the
// caller actually wants to hear has to be saved across it. Same shape as the
// saved_errno dance in platform_file.cpp's sync_directory.
socket_t fail_and_close(socket_t sock) noexcept {
    const int err = last_error_code();
    (void)close_native(sock);
    set_error_code(err);
    return kInvalidSocket;
}

// IPv4 only; see the header. Returns false and sets an error code if `host` is
// not a dotted-quad address.
bool fill_address(const std::string& host, std::uint16_t port, sockaddr_in* out) {
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    // htons/ntohs are unqualified deliberately: the Windows SDK defines them as
    // macros (_byteswap_ushort), and `::htons` is a syntax error once the
    // preprocessor has had it.
    out->sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1) {
#ifdef _WIN32
        set_error_code(WSAEINVAL);
#else
        set_error_code(EINVAL);
#endif
        return false;
    }
    return true;
}

// Turns off Nagle's algorithm on a connected socket.
//
// Nagle holds a small write back until the previous one is acknowledged, so that
// a program writing a byte at a time does not put each on the wire in its own
// 40-byte packet. Combined with the peer's delayed-ACK timer it produces the
// classic ~40ms stall, and it is pure loss for a request/response protocol like
// this one: every message is already written in a single send, and the next
// message does not exist until this one has been answered. There is nothing left
// to coalesce, only latency to add.
void set_no_delay(socket_t sock) noexcept {
    const int on = 1;
    (void)::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&on), sizeof(on));
}

len_t chunk_len(std::size_t count) noexcept {
    return static_cast<len_t>(std::min(count, kMaxChunk));
}

}  // namespace

int socket_startup() {
#ifdef _WIN32
    // A function-local static: the standard guarantees it is initialised exactly
    // once even if several threads reach this line together, which is the normal
    // situation in a thread-per-connection server. The destructor runs at static
    // destruction -- after main() returns, and so after any Server has joined its
    // threads and closed its sockets.
    static WinsockGuard guard;
    return guard.ok ? 0 : -1;
#else
    return 0;  // Nothing to start.
#endif
}

socket_t listen_on(const std::string& host, std::uint16_t port, int backlog) {
    if (socket_startup() != 0) {
        return kInvalidSocket;
    }

    sockaddr_in addr{};
    if (!fill_address(host, port, &addr)) {
        return kInvalidSocket;
    }

    const socket_t sock = static_cast<socket_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!is_valid(sock)) {
        return kInvalidSocket;
    }

    set_reuse_addr(sock);

    if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr),
               static_cast<addrlen_t>(sizeof(addr))) != 0) {
        return fail_and_close(sock);
    }
    if (::listen(sock, backlog) != 0) {
        return fail_and_close(sock);
    }
    return sock;
}

socket_t accept_one(socket_t listener) {
    const socket_t sock = static_cast<socket_t>(::accept(listener, nullptr, nullptr));
    if (!is_valid(sock)) {
        return kInvalidSocket;
    }
    set_no_delay(sock);
    return sock;
}

socket_t connect_to(const std::string& host, std::uint16_t port) {
    if (socket_startup() != 0) {
        return kInvalidSocket;
    }

    sockaddr_in addr{};
    if (!fill_address(host, port, &addr)) {
        return kInvalidSocket;
    }

    const socket_t sock = static_cast<socket_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!is_valid(sock)) {
        return kInvalidSocket;
    }

    if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr),
                  static_cast<addrlen_t>(sizeof(addr))) != 0) {
        return fail_and_close(sock);
    }
    set_no_delay(sock);
    return sock;
}

int close_socket(socket_t sock) { return close_native(sock); }

std::int64_t recv_some(socket_t sock, void* buf, std::size_t count) {
    const auto n = ::recv(sock, static_cast<char*>(buf), chunk_len(count), 0);
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t send_some(socket_t sock, const void* buf, std::size_t count) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    // Writing to a socket the peer has closed raises SIGPIPE, whose default
    // action kills the process. A server must not die because one client hung
    // up mid-response, so ask for the error return (EPIPE) instead of the
    // signal. Windows has no SIGPIPE and needs nothing here.
    flags |= MSG_NOSIGNAL;
#endif
    const auto n = ::send(sock, static_cast<const char*>(buf), chunk_len(count), flags);
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

int shutdown_both(socket_t sock) { return ::shutdown(sock, kShutdownBoth); }

int local_port(socket_t sock) {
    sockaddr_in addr{};
    addrlen_t len = static_cast<addrlen_t>(sizeof(addr));
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return -1;
    }
    return static_cast<int>(ntohs(addr.sin_port));  // Unqualified; see fill_address.
}

int poll_readable(socket_t sock, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = static_cast<SOCKET>(sock);
    pfd.events = POLLRDNORM;
    const int n = ::WSAPoll(&pfd, 1, timeout_ms);
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;
    const int n = ::poll(&pfd, 1, timeout_ms);
    if (n < 0 && errno == EINTR) {
        // A signal interrupted the wait. Nothing is wrong and nothing is ready;
        // report a timeout so the caller loops and re-checks its stop flag,
        // which is what it would have done on a real timeout anyway.
        return 0;
    }
#endif
    return n > 0 ? 1 : n;
}

std::string last_socket_error() {
    const int code = last_error_code();
    if (code == 0) {
        return "unknown error";
    }
    return format_error(code);
}

}  // namespace kvstore::platform
