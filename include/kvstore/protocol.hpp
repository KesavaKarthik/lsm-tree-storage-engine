#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kvstore/result.hpp"
#include "kvstore/socket.hpp"
#include "kvstore/status.hpp"

namespace kvstore::protocol {

// The wire format.
//
// TCP delivers a *byte stream*, not messages. The bytes arrive in order and
// none are lost, and that is the entire guarantee: a peer's single send() of 40
// bytes can surface as one recv() of 40, or three of 17/1/22, or half of it
// glued to the front of the next message. There is no "end of message" in the
// protocol below TCP, so one has to be put in this one. That is all framing is.
//
//   Frame (both directions):
//
//     [ length 4B big-endian ][ payload -- exactly `length` bytes ]
//
//   Request payload:
//
//     [ op 1B ][ key_size 4B BE ][ key ][ value ]
//       0x01 GET   0x02 PUT   0x03 DELETE
//
//   Response payload:
//
//     [ status 1B ][ body ]
//       0 Ok  1 NotFound  2 IOError  3 Corruption  4 InvalidArgument
//
// Why each piece:
//
//   length     -- the message boundary, and the only one there is. A reader
//                 accumulates until it holds 4 bytes, learns how many more to
//                 wait for, and accumulates until it has those. Anything past
//                 that belongs to the next message.
//   op         -- one byte rather than a text verb. There are three of them and
//                 they map one-to-one onto the three KVStore virtuals, so a
//                 parser is a switch rather than a tokeniser.
//   key_size   -- keys are bytes and may contain NULs, so the length is
//                 explicit, exactly as in the on-disk record format.
//   value      -- **the remainder of the frame**, with no length of its own.
//                 The frame length already bounds it, and a second length is a
//                 second source of truth that can disagree with the first --
//                 which is then a decision about which one to believe. GET and
//                 DELETE carry no value bytes at all.
//   status     -- a 1:1 map of StatusCode. No network-specific code was added:
//                 a socket is an I/O device and a failed one is an IOError, so
//                 the five codes the engine already has cover the wire too.
//   body       -- the value on a successful GET; Status::message() on any
//                 failure, so a client has something to print; empty otherwise.
//
// Two rules a reader must not skip, both of which are the network restatement
// of what Bitcask::recover does with a record header off disk:
//
//   * **Bound every size before trusting it.** key_size arrives from a stranger
//     and can be any 32-bit number. Check it against the bytes that actually
//     arrived before indexing with it.
//   * **Cap the frame length.** Four bytes lets a peer ask for a 4GB allocation
//     with five bytes of effort. kMaxFrameSize is where that request stops
//     being honoured.

// Enough for a large value, small enough that a hostile length prefix cannot
// exhaust memory. Requests over this are refused without allocating for them.
inline constexpr std::uint32_t kMaxFrameSize = 8u << 20;

inline constexpr std::size_t kLengthPrefixSize = 4;
inline constexpr std::size_t kRequestHeaderSize = 5;   // op 1 + key_size 4
inline constexpr std::size_t kResponseHeaderSize = 1;  // status 1

enum class Op : std::uint8_t {
    Get = 0x01,
    Put = 0x02,
    Delete = 0x03,
};

struct Request {
    Op op = Op::Get;
    std::string key;
    std::string value;  // Empty for Get and Delete.
};

struct Response {
    StatusCode code = StatusCode::Ok;
    std::string body;
};

[[nodiscard]] std::uint8_t status_byte(StatusCode code) noexcept;

// InvalidArgument if the byte is not one of the five defined codes.
[[nodiscard]] Result<StatusCode> status_from_byte(std::uint8_t byte);

// Rebuilds the Status a response is reporting, message included. What a client
// hands back to its caller.
[[nodiscard]] Status to_status(const Response& response);

// --- Payload codec ----------------------------------------------------------
//
// These deal in payloads only; the length prefix belongs to the frame layer
// below and never appears here. That split is what lets the codec be tested
// against literal expected bytes with no socket anywhere near it.

[[nodiscard]] std::vector<std::uint8_t> encode_request(const Request& request);
[[nodiscard]] std::vector<std::uint8_t> encode_response(const Response& response);

// A malformed payload is InvalidArgument, not Corruption. The distinction is
// load-bearing for the server: InvalidArgument means *this message* was
// nonsense and gets an error response, with the connection left open;
// Corruption is reserved for the framing itself going wrong, after which the
// stream position is unknown and the connection cannot be continued.
[[nodiscard]] Result<Request> decode_request(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<Response> decode_response(std::span<const std::uint8_t> payload);

// --- Frame layer ------------------------------------------------------------

// Prepends the length and sends the whole thing.
//
// Builds one buffer and makes one send_all call rather than sending the prefix
// and the payload separately. Two sends with Nagle disabled is two packets --
// a 4-byte one and a large one -- for a message that fits comfortably in either.
[[nodiscard]] Status write_frame(Socket& sock, std::span<const std::uint8_t> payload);

enum class FrameStatus {
    Complete,    // *payload holds exactly one frame's contents.
    PeerClosed,  // The peer closed cleanly, between frames. Not an error.
    Cancelled,   // Asked to give up while waiting. See ReadPolicy.
};

// How a reader behaves while it is waiting for bytes that have not arrived.
//
// **Why this exists at all.** A connection thread blocked in recv() cannot be
// interrupted. The tempting answer is for the shutdown path to call shutdown()
// on the socket from another thread and have the blocked recv return -- and that
// answer is wrong, because Winsock does not honour it: the recv stays blocked
// for about two minutes while the POSIX build looks perfect. There is no
// portable way to cancel a blocking read.
//
// So the reader does not do a blocking read. It waits with a timeout, and every
// time the timeout expires it asks whether it should still be here. Nothing has
// to interrupt anything, which is why it works the same everywhere.
//
// The default -- no timeout, no flag -- blocks indefinitely, which is what a
// client wants: it has just sent a request and has nothing to do but wait.
struct ReadPolicy {
    // Milliseconds to wait for bytes before re-checking `cancelled`. Zero or
    // less means block indefinitely and never check.
    int poll_timeout_ms = 0;

    // Polled between waits. When it reads true, read_frame gives up and returns
    // FrameStatus::Cancelled. Not owned; must outlive the reader.
    const std::atomic<bool>* cancelled = nullptr;
};

// Reassembles frames from a socket's byte stream.
//
// This is a class rather than a free function because it has to hold state
// between calls, and that state is the whole problem. A single recv() can return
// half a frame -- so the reader keeps what it has and asks for more -- and it can
// equally return two whole frames at once, so the reader keeps the surplus,
// which belongs to the *next* call. A free function reading "one message" would
// have to either throw those bytes away or read one byte at a time; the first is
// a bug that only shows up under load, and the second is a syscall per byte.
//
// One reader per connection, living as long as the connection does.
class FrameReader {
public:
    explicit FrameReader(Socket& sock, std::uint32_t max_frame = kMaxFrameSize,
                         ReadPolicy policy = {}) noexcept
        : sock_(sock), max_frame_(max_frame), policy_(policy) {}

    // Reads until one complete frame is available, and hands over its payload.
    //
    // The three outcomes are deliberately distinct, and the distinction is the
    // same one Bitcask::recover draws between a torn tail and a damaged file:
    //
    //   Ok/Complete   -- a whole frame.
    //   Ok/PeerClosed -- the peer stopped sending *exactly at a frame boundary*.
    //                    An ordinary hang-up; the connection just ends.
    //   Ok/Cancelled  -- the ReadPolicy's flag was raised while waiting. The
    //                    connection is fine; this process is going away.
    //   Corruption    -- the peer stopped sending *part-way through* a frame, or
    //                    announced a length past the cap. The stream position is
    //                    no longer known, so the connection cannot continue.
    [[nodiscard]] Result<FrameStatus> read_frame(std::vector<std::uint8_t>* payload);

private:
    enum class Fill {
        Filled,
        PeerClosed,  // Zero bytes. Whether that is clean depends on where we are.
        Cancelled,
    };

    // Reads until the buffer holds at least `wanted` bytes.
    [[nodiscard]] Result<Fill> fill_to(std::size_t wanted);

    Socket& sock_;
    std::uint32_t max_frame_;
    ReadPolicy policy_;

    // Bytes received but not yet handed out. Usually empty between calls; holds
    // the start of the next frame when one recv delivered more than one message.
    std::vector<std::uint8_t> buf_;
};

}  // namespace kvstore::protocol
