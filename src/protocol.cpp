#include "kvstore/protocol.hpp"

#include <algorithm>
#include <string>

#include "wire.hpp"

namespace kvstore::protocol {
namespace {

void append_string(std::vector<std::uint8_t>& out, const std::string& s) {
    // reinterpret_cast rather than iterator conversion: char to uint8_t is a
    // narrowing conversion as far as the compiler is concerned, and /W4 says so
    // on every one of the bytes. The bytes are the same bytes either way.
    const auto* first = reinterpret_cast<const std::uint8_t*>(s.data());
    out.insert(out.end(), first, first + s.size());
}

std::string to_string(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        // An empty span's data() may be null, and std::string(nullptr, 0) is
        // undefined rather than empty.
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const char* op_name(Op op) noexcept {
    switch (op) {
        case Op::Get:    return "GET";
        case Op::Put:    return "PUT";
        case Op::Delete: return "DELETE";
    }
    return "?";
}

}  // namespace

std::uint8_t status_byte(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok:              return 0;
        case StatusCode::NotFound:        return 1;
        case StatusCode::IOError:         return 2;
        case StatusCode::Corruption:      return 3;
        case StatusCode::InvalidArgument: return 4;
    }
    return 2;  // Unreachable. IOError is the least misleading "went wrong".
}

Result<StatusCode> status_from_byte(std::uint8_t byte) {
    switch (byte) {
        case 0: return StatusCode::Ok;
        case 1: return StatusCode::NotFound;
        case 2: return StatusCode::IOError;
        case 3: return StatusCode::Corruption;
        case 4: return StatusCode::InvalidArgument;
        default: break;
    }
    return Status::invalid_argument("unknown status byte " + std::to_string(byte));
}

Status to_status(const Response& response) {
    switch (response.code) {
        case StatusCode::Ok:              return Status::ok();
        case StatusCode::NotFound:        return Status::not_found(response.body);
        case StatusCode::IOError:         return Status::io_error(response.body);
        case StatusCode::Corruption:      return Status::corruption(response.body);
        case StatusCode::InvalidArgument: return Status::invalid_argument(response.body);
    }
    return Status::io_error("unknown status code from server");
}

std::vector<std::uint8_t> encode_request(const Request& request) {
    std::vector<std::uint8_t> out;
    out.reserve(kRequestHeaderSize + request.key.size() + request.value.size());

    out.push_back(static_cast<std::uint8_t>(request.op));
    wire::append_u32(out, static_cast<std::uint32_t>(request.key.size()));
    append_string(out, request.key);

    // No length for the value: the frame bounds it. See the header.
    if (request.op == Op::Put) {
        append_string(out, request.value);
    }
    return out;
}

std::vector<std::uint8_t> encode_response(const Response& response) {
    std::vector<std::uint8_t> out;
    out.reserve(kResponseHeaderSize + response.body.size());

    out.push_back(status_byte(response.code));
    append_string(out, response.body);
    return out;
}

Result<Request> decode_request(std::span<const std::uint8_t> payload) {
    if (payload.size() < kRequestHeaderSize) {
        return Status::invalid_argument("request payload is " + std::to_string(payload.size()) +
                                        " bytes, shorter than its " +
                                        std::to_string(kRequestHeaderSize) + "-byte header");
    }

    Request request;
    switch (payload[0]) {
        case static_cast<std::uint8_t>(Op::Get):    request.op = Op::Get; break;
        case static_cast<std::uint8_t>(Op::Put):    request.op = Op::Put; break;
        case static_cast<std::uint8_t>(Op::Delete): request.op = Op::Delete; break;
        default:
            return Status::invalid_argument("unknown op byte " + std::to_string(payload[0]));
    }

    const std::uint32_t key_size = wire::get_u32(payload, 1);

    // key_size came from a stranger and can be any 32-bit number, so it is
    // bounded against what actually arrived before it is used to index. The
    // subtraction cannot underflow: the length check above guarantees the
    // payload is at least kRequestHeaderSize.
    const std::size_t after_header = payload.size() - kRequestHeaderSize;
    if (key_size > after_header) {
        return Status::invalid_argument("key_size " + std::to_string(key_size) +
                                        " exceeds the " + std::to_string(after_header) +
                                        " bytes left in the frame");
    }

    request.key = to_string(payload.subspan(kRequestHeaderSize, key_size));

    const std::span<const std::uint8_t> rest = payload.subspan(kRequestHeaderSize + key_size);
    if (request.op == Op::Put) {
        request.value = to_string(rest);
    } else if (!rest.empty()) {
        // Rejected rather than ignored. Trailing bytes on a GET mean the sender
        // and this parser disagree about the format, and the disagreement is
        // cheaper to hear about now than to discover on the message after it.
        return Status::invalid_argument(std::string{op_name(request.op)} + " carries " +
                                        std::to_string(rest.size()) +
                                        " unexpected trailing bytes");
    }

    return request;
}

Result<Response> decode_response(std::span<const std::uint8_t> payload) {
    if (payload.empty()) {
        return Status::invalid_argument("response payload is empty");
    }

    auto code = status_from_byte(payload[0]);
    if (!code.is_ok()) {
        return code.status();
    }

    Response response;
    response.code = *code;
    response.body = to_string(payload.subspan(kResponseHeaderSize));
    return response;
}

// --- Frame layer ------------------------------------------------------------

namespace {

// How much to ask the kernel for when the reader needs less than this. Reading
// only what is strictly needed would be a syscall for the 4-byte prefix and
// another for the payload of every message; asking for a page at a time usually
// collects both at once -- and, on a busy connection, the message after them.
// The surplus is not waste, it is the next frame arriving early.
constexpr std::size_t kReadChunk = 4096;

}  // namespace

Status write_frame(Socket& sock, std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaxFrameSize) {
        return Status::invalid_argument("frame payload of " + std::to_string(payload.size()) +
                                        " bytes exceeds the " + std::to_string(kMaxFrameSize) +
                                        "-byte cap");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(kLengthPrefixSize + payload.size());
    wire::append_u32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    return sock.send_all(frame);
}

Result<FrameReader::Fill> FrameReader::fill_to(std::size_t wanted) {
    while (buf_.size() < wanted) {
        // Wait with a timeout rather than blocking in recv, so that a thread
        // sitting on an idle connection still comes up for air often enough to
        // be told to stop. There is no portable way to interrupt a blocking
        // recv -- so it is never entered without knowing bytes are there.
        if (policy_.poll_timeout_ms > 0) {
            auto ready = sock_.wait_readable(policy_.poll_timeout_ms);
            if (!ready.is_ok()) {
                return ready.status();
            }
            if (!*ready) {
                if (policy_.cancelled != nullptr &&
                    policy_.cancelled->load(std::memory_order_relaxed)) {
                    return Fill::Cancelled;
                }
                continue;  // Nothing yet, and nobody has asked us to leave.
            }
        }

        const std::size_t have = buf_.size();
        const std::size_t ask = std::max(wanted - have, kReadChunk);

        // Grow first and shrink back to what actually arrived: recv needs
        // somewhere to write, and how much it will write is not knowable in
        // advance -- that is the entire nature of the thing.
        buf_.resize(have + ask);
        auto got = sock_.recv_some(std::span{buf_}.subspan(have));
        if (!got.is_ok()) {
            buf_.resize(have);
            return got.status();
        }
        buf_.resize(have + *got);

        if (*got == 0) {
            return Fill::PeerClosed;  // Whether that is clean is not decided here.
        }
    }
    return Fill::Filled;
}

Result<FrameStatus> FrameReader::read_frame(std::vector<std::uint8_t>* payload) {
    payload->clear();

    // 1. The length prefix. This may take several recvs, or none at all -- the
    //    previous call may have pulled this frame's prefix in behind the last
    //    one, which is exactly why buf_ survives between calls.
    auto have_prefix = fill_to(kLengthPrefixSize);
    if (!have_prefix.is_ok()) {
        return have_prefix.status();
    }
    if (*have_prefix == Fill::Cancelled) {
        return FrameStatus::Cancelled;
    }
    if (*have_prefix == Fill::PeerClosed) {
        // The peer stopped sending. *Where* it stopped is the whole question.
        if (buf_.empty()) {
            // Exactly at a frame boundary: an ordinary hang-up, and the only
            // way a well-behaved client ever ends a conversation.
            return FrameStatus::PeerClosed;
        }
        // Part-way through a prefix: the message was cut off. Nothing can be
        // recovered from a stream whose position is unknown.
        return Status::corruption("connection ended after " + std::to_string(buf_.size()) +
                                  " of " + std::to_string(kLengthPrefixSize) +
                                  " length-prefix bytes");
    }

    const std::uint32_t length = wire::get_u32(buf_, 0);

    // Checked *before* anything is sized from it. Four bytes let a peer ask for
    // a 4GB allocation with five bytes of effort, and a server that honours the
    // request is a server anyone can switch off from a shell prompt.
    if (length > max_frame_) {
        return Status::corruption("frame length " + std::to_string(length) + " exceeds the " +
                                  std::to_string(max_frame_) + "-byte cap");
    }

    // 2. The payload.
    const std::size_t total = kLengthPrefixSize + length;
    auto have_payload = fill_to(total);
    if (!have_payload.is_ok()) {
        return have_payload.status();
    }
    if (*have_payload == Fill::Cancelled) {
        // Mid-frame, but that costs nothing: the connection is being dropped, so
        // there is no next message to have lost our place in.
        return FrameStatus::Cancelled;
    }
    if (*have_payload == Fill::PeerClosed) {
        return Status::corruption("connection ended after " +
                                  std::to_string(buf_.size() - kLengthPrefixSize) + " of " +
                                  std::to_string(length) + " payload bytes");
    }

    payload->assign(buf_.begin() + static_cast<std::ptrdiff_t>(kLengthPrefixSize),
                    buf_.begin() + static_cast<std::ptrdiff_t>(total));

    // Whatever is left belongs to the next frame. Dropping it here is the other
    // half of the framing bug -- less famous than the partial read, and just as
    // real: one recv can carry two messages, and the second one would vanish.
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));

    return FrameStatus::Complete;
}

}  // namespace kvstore::protocol
