#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "kvstore/protocol.hpp"
#include "kvstore/status.hpp"

// The payload codec on its own -- no sockets, no threads, no framing. Byte order
// is asserted against literal expected bytes rather than by round-tripping,
// because a round trip through a byte-swapped encoder and its matching decoder
// succeeds perfectly and proves nothing.

namespace kvstore {
namespace {

using protocol::Op;
using protocol::Request;
using protocol::Response;

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    out.reserve(values.size());
    for (const int v : values) {
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

// --- Requests, byte for byte ------------------------------------------------

TEST(ProtocolRequestTest, EncodesGetAsExpectedBytes) {
    const Request request{Op::Get, "ab", ""};

    EXPECT_EQ(protocol::encode_request(request),
              bytes({0x01,                    // op = GET
                     0x00, 0x00, 0x00, 0x02,  // key_size = 2, big-endian
                     'a', 'b'}));
}

TEST(ProtocolRequestTest, EncodesPutAsExpectedBytes) {
    const Request request{Op::Put, "ab", "xyz"};

    EXPECT_EQ(protocol::encode_request(request),
              bytes({0x02,                    // op = PUT
                     0x00, 0x00, 0x00, 0x02,  // key_size = 2
                     'a', 'b',                // key
                     'x', 'y', 'z'}));        // value: the rest of the frame
}

TEST(ProtocolRequestTest, EncodesDeleteAsExpectedBytes) {
    const Request request{Op::Delete, "k", ""};

    EXPECT_EQ(protocol::encode_request(request), bytes({0x03, 0x00, 0x00, 0x00, 0x01, 'k'}));
}

TEST(ProtocolRequestTest, KeySizeIsBigEndian) {
    // 258 == 0x00000102. Big-endian puts the most significant byte first, so the
    // 0x01 precedes the 0x02. A little-endian encoder writes 02 01 00 00 here,
    // and would still round-trip through its own decoder -- which is exactly why
    // this is asserted against literal bytes.
    const Request request{Op::Get, std::string(258, 'k'), ""};

    const std::vector<std::uint8_t> encoded = protocol::encode_request(request);
    ASSERT_GE(encoded.size(), 5u);
    EXPECT_EQ(encoded[1], 0x00);
    EXPECT_EQ(encoded[2], 0x00);
    EXPECT_EQ(encoded[3], 0x01);
    EXPECT_EQ(encoded[4], 0x02);
}

TEST(ProtocolRequestTest, PutIgnoresNothingAndValueNeedsNoLength) {
    // The value is whatever is left after the key, so a value containing bytes
    // that look like a length field is not special in any way.
    const Request request{Op::Put, "k", std::string("\x00\x00\x00\x09", 4)};

    auto decoded = protocol::decode_request(protocol::encode_request(request));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->value.size(), 4u);
    EXPECT_EQ(decoded->value, request.value);
}

// --- Requests, round trips --------------------------------------------------

TEST(ProtocolRequestTest, RoundTripsEveryOp) {
    for (const Request& request : {Request{Op::Get, "alpha", ""},
                                   Request{Op::Put, "alpha", "one"},
                                   Request{Op::Delete, "alpha", ""}}) {
        auto decoded = protocol::decode_request(protocol::encode_request(request));
        ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
        EXPECT_EQ(decoded->op, request.op);
        EXPECT_EQ(decoded->key, request.key);
        EXPECT_EQ(decoded->value, request.value);
    }
}

TEST(ProtocolRequestTest, KeysAndValuesAreBinarySafe) {
    // The reason key_size is explicit and the value is length-delimited by the
    // frame: neither is a C string and either may contain NULs.
    const Request request{Op::Put, std::string("k\0ey", 4), std::string("a\0b\0c", 5)};

    auto decoded = protocol::decode_request(protocol::encode_request(request));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->key, request.key);
    EXPECT_EQ(decoded->key.size(), 4u);
    EXPECT_EQ(decoded->value, request.value);
    EXPECT_EQ(decoded->value.size(), 5u);
}

TEST(ProtocolRequestTest, EmptyValuePutRoundTrips) {
    const Request request{Op::Put, "alpha", ""};

    auto decoded = protocol::decode_request(protocol::encode_request(request));
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->op, Op::Put);
    EXPECT_EQ(decoded->value, "");
}

TEST(ProtocolRequestTest, EmptyKeySurvivesTheCodec) {
    // The protocol does not pre-judge this. An empty key is a well-formed frame;
    // whether it is a legal *request* is the engine's ruling, and routing it
    // through keeps one source of truth for the KVStore contract.
    const Request request{Op::Put, "", "value"};

    auto decoded = protocol::decode_request(protocol::encode_request(request));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->key, "");
    EXPECT_EQ(decoded->value, "value");
}

// --- Requests, malformed ----------------------------------------------------

TEST(ProtocolRequestTest, RejectsPayloadShorterThanTheHeader) {
    auto decoded = protocol::decode_request(bytes({0x01, 0x00, 0x00}));
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_invalid_argument()) << decoded.status().to_string();
}

TEST(ProtocolRequestTest, RejectsEmptyPayload) {
    auto decoded = protocol::decode_request({});
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_invalid_argument());
}

TEST(ProtocolRequestTest, RejectsUnknownOp) {
    auto decoded = protocol::decode_request(bytes({0x7f, 0x00, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_invalid_argument()) << decoded.status().to_string();
}

TEST(ProtocolRequestTest, RejectsKeySizeLargerThanTheFrame) {
    // A hand-built payload claiming a 16-byte key with 2 bytes behind it. The
    // decoder must bound the claimed size against what actually arrived rather
    // than reading past the buffer it was handed.
    auto decoded = protocol::decode_request(bytes({0x01, 0x00, 0x00, 0x00, 0x10, 'a', 'b'}));
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_invalid_argument()) << decoded.status().to_string();
}

TEST(ProtocolRequestTest, RejectsAnEnormousKeySizeWithoutReadingIt) {
    // 0xFFFFFFFF. The bound check has to happen before this is used for
    // anything, and the subtraction it does must not underflow.
    auto decoded =
        protocol::decode_request(bytes({0x01, 0xFF, 0xFF, 0xFF, 0xFF, 'a'}));
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_invalid_argument()) << decoded.status().to_string();
}

TEST(ProtocolRequestTest, RejectsTrailingBytesOnGetAndDelete) {
    // key_size 1, key 'a', then a stray byte. GET has no value field, so this is
    // the sender and the parser disagreeing about the format.
    for (const int op : {0x01, 0x03}) {
        auto decoded = protocol::decode_request(bytes({op, 0x00, 0x00, 0x00, 0x01, 'a', 'z'}));
        EXPECT_FALSE(decoded.is_ok());
        EXPECT_TRUE(decoded.status().is_invalid_argument()) << decoded.status().to_string();
    }
}

// --- Responses --------------------------------------------------------------

TEST(ProtocolResponseTest, EncodesAsExpectedBytes) {
    EXPECT_EQ(protocol::encode_response(Response{StatusCode::Ok, "one"}),
              bytes({0x00, 'o', 'n', 'e'}));
    EXPECT_EQ(protocol::encode_response(Response{StatusCode::NotFound, ""}), bytes({0x01}));
}

TEST(ProtocolResponseTest, EveryStatusCodeHasADistinctByteAndRoundTrips) {
    // The wire byte is a 1:1 map of StatusCode. If a code is ever added to the
    // enum without a byte, this is what notices.
    for (const StatusCode code : {StatusCode::Ok, StatusCode::NotFound, StatusCode::IOError,
                                  StatusCode::Corruption, StatusCode::InvalidArgument}) {
        const std::uint8_t byte = protocol::status_byte(code);
        auto back = protocol::status_from_byte(byte);
        ASSERT_TRUE(back.is_ok()) << "byte " << static_cast<int>(byte);
        EXPECT_EQ(*back, code);
    }
}

TEST(ProtocolResponseTest, RejectsAnUndefinedStatusByte) {
    auto code = protocol::status_from_byte(9);
    EXPECT_FALSE(code.is_ok());
    EXPECT_TRUE(code.status().is_invalid_argument());

    auto decoded = protocol::decode_response(bytes({9, 'x'}));
    EXPECT_FALSE(decoded.is_ok());
}

TEST(ProtocolResponseTest, RejectsEmptyPayload) {
    auto decoded = protocol::decode_response({});
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_TRUE(decoded.status().is_invalid_argument());
}

TEST(ProtocolResponseTest, RoundTripsAValueBody) {
    const Response response{StatusCode::Ok, std::string("a\0b", 3)};

    auto decoded = protocol::decode_response(protocol::encode_response(response));
    ASSERT_TRUE(decoded.is_ok()) << decoded.status().to_string();
    EXPECT_EQ(decoded->code, StatusCode::Ok);
    EXPECT_EQ(decoded->body, response.body);
    EXPECT_EQ(decoded->body.size(), 3u);
}

TEST(ProtocolResponseTest, CarriesTheStatusMessageBackToTheClient) {
    // Why the body doubles as the error text: it is what makes kv-cli able to
    // print something better than a number.
    const Status original = Status::not_found("no such key: alpha");
    const Response response{original.code(), original.message()};

    auto decoded = protocol::decode_response(protocol::encode_response(response));
    ASSERT_TRUE(decoded.is_ok());

    const Status rebuilt = protocol::to_status(*decoded);
    EXPECT_TRUE(rebuilt.is_not_found());
    EXPECT_EQ(rebuilt.to_string(), original.to_string());
}

TEST(ProtocolResponseTest, ToStatusOfAnOkResponseIsOk) {
    EXPECT_TRUE(protocol::to_status(Response{StatusCode::Ok, "the value"}).is_ok());
}

}  // namespace
}  // namespace kvstore
