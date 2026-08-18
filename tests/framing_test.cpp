#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/protocol.hpp"
#include "kvstore/socket.hpp"
#include "kvstore/status.hpp"
#include "kvstore_contract.hpp"  // EXPECT_OK / EXPECT_STATUS.
#include "socket_helpers.hpp"

// Turning a byte stream back into messages.
//
// Every case here exists because TCP promises to deliver the bytes in order and
// promises *nothing at all* about how they are grouped. The sender's calls to
// send() are not message boundaries, so a correct reader has to work for any
// grouping the network cares to produce -- and these tests produce the awkward
// ones deliberately, since a loopback connection with small messages will
// otherwise hand over one whole frame per recv every single time and let a
// completely broken reader pass.

namespace kvstore {
namespace {

using testing_support::as_string;
using testing_support::ConnectedPair;
using testing_support::make_pair;

// A frame built by hand rather than by write_frame, so the tests below own the
// bytes they are asserting about and can chop them up.
std::vector<std::uint8_t> framed(const std::string& payload) {
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> out{
        static_cast<std::uint8_t>(n >> 24),
        static_cast<std::uint8_t>(n >> 16),
        static_cast<std::uint8_t>(n >> 8),
        static_cast<std::uint8_t>(n),
    };
    const auto* first = reinterpret_cast<const std::uint8_t*>(payload.data());
    out.insert(out.end(), first, first + payload.size());
    return out;
}

// Sends bytes[from, to) as one send() call.
void send_slice(Socket& sock, const std::vector<std::uint8_t>& bytes, std::size_t from,
                std::size_t to) {
    EXPECT_OK(sock.send_all(std::span{bytes}.subspan(from, to - from)));
}

void pause_briefly() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

// --- The happy path ---------------------------------------------------------

TEST(FramingTest, RoundTripsOneFrame) {
    ConnectedPair pair = make_pair();
    EXPECT_OK(protocol::write_frame(pair.client, testing_support::as_bytes("hello")));

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
    EXPECT_EQ(*outcome, protocol::FrameStatus::Complete);
    EXPECT_EQ(as_string(payload), "hello");
}

TEST(FramingTest, RoundTripsAnEmptyFrame) {
    // Length 0 is a legal frame: four bytes of prefix and nothing after them.
    // Worth pinning down because it is the one payload size where "read until
    // you have `length` bytes" reads nothing at all.
    ConnectedPair pair = make_pair();
    EXPECT_OK(protocol::write_frame(pair.client, {}));

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
    EXPECT_EQ(*outcome, protocol::FrameStatus::Complete);
    EXPECT_TRUE(payload.empty());
}

TEST(FramingTest, RoundTripsBinaryPayloadWithNuls) {
    ConnectedPair pair = make_pair();
    const std::string blob("\x00\x01\x00\xff\x00", 5);
    EXPECT_OK(protocol::write_frame(pair.client, testing_support::as_bytes(blob)));

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    ASSERT_TRUE(outcome.is_ok());
    EXPECT_EQ(payload.size(), 5u);
    EXPECT_EQ(as_string(payload), blob);
}

// --- Partial reads ----------------------------------------------------------

TEST(FramingTest, DeliversAMessageSplitAcrossThreeSends) {
    // The case the whole design exists for. The reader must never assume one
    // recv is one message: here the first send carries a *single byte* of the
    // length prefix, so a reader that decodes a length from whatever arrived
    // first reads a nonsense number and desynchronises permanently.
    ConnectedPair pair = make_pair();

    const std::string payload(500, 'p');
    const std::vector<std::uint8_t> frame = framed(payload);

    std::vector<std::uint8_t> got;
    auto outcome = protocol::FrameStatus::PeerClosed;

    // The reader runs concurrently and blocks in recv between chunks, which is
    // what forces each send below to be seen as its own arrival.
    std::thread reader{[&] {
        protocol::FrameReader r{pair.server};
        auto result = r.read_frame(&got);
        ASSERT_TRUE(result.is_ok()) << result.status().to_string();
        outcome = *result;
    }};

    send_slice(pair.client, frame, 0, 1);  // One byte of the 4-byte prefix.
    pause_briefly();
    send_slice(pair.client, frame, 1, 4 + 250);  // Rest of the prefix, half the payload.
    pause_briefly();
    send_slice(pair.client, frame, 4 + 250, frame.size());  // The remainder.

    reader.join();

    EXPECT_EQ(outcome, protocol::FrameStatus::Complete);
    EXPECT_EQ(as_string(got), payload);
}

TEST(FramingTest, DeliversAMessageSentOneByteAtATime) {
    // The pathological grouping. Nothing about the protocol should care.
    ConnectedPair pair = make_pair();

    const std::string payload = "one byte at a time";
    const std::vector<std::uint8_t> frame = framed(payload);

    std::vector<std::uint8_t> got;
    std::thread reader{[&] {
        protocol::FrameReader r{pair.server};
        auto result = r.read_frame(&got);
        ASSERT_TRUE(result.is_ok()) << result.status().to_string();
        EXPECT_EQ(*result, protocol::FrameStatus::Complete);
    }};

    for (std::size_t i = 0; i < frame.size(); ++i) {
        send_slice(pair.client, frame, i, i + 1);
    }

    reader.join();
    EXPECT_EQ(as_string(got), payload);
}

TEST(FramingTest, ReassemblesAFrameTooLargeForOneRecv) {
    // A megabyte cannot arrive in one recv, so the payload loop runs for real
    // rather than being satisfied on its first pass.
    ConnectedPair pair = make_pair();
    const std::string payload(1u << 20, 'x');

    std::thread writer{
        [&] { EXPECT_OK(protocol::write_frame(pair.client, testing_support::as_bytes(payload))); }};

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> got;
    auto outcome = reader.read_frame(&got);

    writer.join();

    ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
    EXPECT_EQ(got.size(), payload.size());
    EXPECT_EQ(as_string(got), payload);
}

// --- Surplus bytes ----------------------------------------------------------

TEST(FramingTest, ReadsTwoFramesDeliveredInOneSend) {
    // The other half of the framing bug, and the less famous one: a single recv
    // can carry more than one message. A reader that discards whatever is left
    // in its buffer after a frame loses the second message entirely -- and does
    // so only under the load that makes messages arrive together.
    ConnectedPair pair = make_pair();

    std::vector<std::uint8_t> both = framed("first");
    const std::vector<std::uint8_t> second = framed("second");
    both.insert(both.end(), second.begin(), second.end());

    EXPECT_OK(pair.client.send_all(both));  // One send, two messages.

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;

    ASSERT_TRUE(reader.read_frame(&payload).is_ok());
    EXPECT_EQ(as_string(payload), "first");

    ASSERT_TRUE(reader.read_frame(&payload).is_ok());
    EXPECT_EQ(as_string(payload), "second");
}

TEST(FramingTest, ReadsManyFramesDeliveredInOneSend) {
    ConnectedPair pair = make_pair();

    constexpr int kCount = 20;
    std::vector<std::uint8_t> all;
    for (int i = 0; i < kCount; ++i) {
        const std::vector<std::uint8_t> one = framed("message" + std::to_string(i));
        all.insert(all.end(), one.begin(), one.end());
    }
    EXPECT_OK(pair.client.send_all(all));

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    for (int i = 0; i < kCount; ++i) {
        auto outcome = reader.read_frame(&payload);
        ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
        EXPECT_EQ(as_string(payload), "message" + std::to_string(i));
    }
}

TEST(FramingTest, KeepsTheSecondFrameWhenItArrivesGluedToTheFirst) {
    // A frame and a half in one send, then the rest. The reader has to hold on
    // to the half and finish it with what comes next.
    ConnectedPair pair = make_pair();

    const std::vector<std::uint8_t> first = framed("alpha");
    const std::vector<std::uint8_t> second = framed("beta");

    std::vector<std::uint8_t> head = first;
    head.insert(head.end(), second.begin(), second.begin() + 3);  // Part of the next prefix.

    std::vector<std::uint8_t> payload;
    std::thread reader{[&] {
        protocol::FrameReader r{pair.server};
        ASSERT_TRUE(r.read_frame(&payload).is_ok());
        EXPECT_EQ(as_string(payload), "alpha");
        ASSERT_TRUE(r.read_frame(&payload).is_ok());
        EXPECT_EQ(as_string(payload), "beta");
    }};

    EXPECT_OK(pair.client.send_all(head));
    pause_briefly();
    send_slice(pair.client, second, 3, second.size());

    reader.join();
}

// --- Ending a connection ----------------------------------------------------

TEST(FramingTest, ReportsACleanDisconnectAtAFrameBoundary) {
    // Zero bytes with nothing buffered is how every well-behaved client ends a
    // conversation. It must not look like an error, or every normal hang-up
    // logs one.
    ConnectedPair pair = make_pair();
    EXPECT_OK(pair.client.close());

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
    EXPECT_EQ(*outcome, protocol::FrameStatus::PeerClosed);
}

TEST(FramingTest, ReportsACleanDisconnectAfterACompleteFrame) {
    ConnectedPair pair = make_pair();
    EXPECT_OK(protocol::write_frame(pair.client, testing_support::as_bytes("last")));
    EXPECT_OK(pair.client.close());

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;

    auto first = reader.read_frame(&payload);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(*first, protocol::FrameStatus::Complete);
    EXPECT_EQ(as_string(payload), "last");

    auto second = reader.read_frame(&payload);
    ASSERT_TRUE(second.is_ok()) << second.status().to_string();
    EXPECT_EQ(*second, protocol::FrameStatus::PeerClosed);
}

TEST(FramingTest, ATruncatedLengthPrefixIsCorruptionNotACleanClose) {
    // Two bytes of a prefix and then a hang-up. This is the network's version of
    // the torn tail Bitcask::recover finds at the end of a log: the difference
    // between "the peer finished" and "the peer was cut off" is *where* it
    // stopped, and only the reader knows that.
    ConnectedPair pair = make_pair();
    const std::vector<std::uint8_t> frame = framed("ignored");
    EXPECT_OK(pair.client.send_all(std::span{frame}.first(2)));
    EXPECT_OK(pair.client.close());

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    EXPECT_FALSE(outcome.is_ok());
    EXPECT_TRUE(outcome.status().is_corruption()) << outcome.status().to_string();
}

TEST(FramingTest, ATruncatedPayloadIsCorruption) {
    ConnectedPair pair = make_pair();

    const std::vector<std::uint8_t> frame = framed("a complete payload");
    EXPECT_OK(pair.client.send_all(std::span{frame}.first(4 + 5)));  // Prefix + 5 of 18.
    EXPECT_OK(pair.client.close());

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    EXPECT_FALSE(outcome.is_ok());
    EXPECT_TRUE(outcome.status().is_corruption()) << outcome.status().to_string();
}

// --- Giving up ---------------------------------------------------------------

TEST(FramingTest, GivesUpOnAnIdleConnectionWhenAsked) {
    // What makes a connection thread joinable. The peer is alive and simply has
    // nothing to say, so there is nothing to time out and nothing to fail -- the
    // reader has to be told, and has to be somewhere it can be told.
    ConnectedPair pair = make_pair();

    std::atomic<bool> stop{false};
    protocol::FrameReader reader{pair.server, protocol::kMaxFrameSize,
                                 protocol::ReadPolicy{/*poll_timeout_ms=*/20, &stop}};

    std::vector<std::uint8_t> payload;
    std::thread waiting{[&] {
        auto outcome = reader.read_frame(&payload);
        ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
        EXPECT_EQ(*outcome, protocol::FrameStatus::Cancelled);
    }};

    pause_briefly();
    stop.store(true);
    waiting.join();  // Hangs if the reader was blocked in recv rather than waiting.
}

TEST(FramingTest, GivesUpPartWayThroughAFrameWhenAsked) {
    // The same, with the peer having sent half a message and then stalled. The
    // reader is mid-frame and still has to be reachable.
    ConnectedPair pair = make_pair();

    const std::vector<std::uint8_t> frame = framed(std::string(200, 'x'));
    EXPECT_OK(pair.client.send_all(std::span{frame}.first(20)));

    std::atomic<bool> stop{false};
    protocol::FrameReader reader{pair.server, protocol::kMaxFrameSize,
                                 protocol::ReadPolicy{/*poll_timeout_ms=*/20, &stop}};

    std::vector<std::uint8_t> payload;
    std::thread waiting{[&] {
        auto outcome = reader.read_frame(&payload);
        ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
        EXPECT_EQ(*outcome, protocol::FrameStatus::Cancelled);
    }};

    pause_briefly();
    stop.store(true);
    waiting.join();
}

TEST(FramingTest, APolicyWithATimeoutStillDeliversFramesNormally) {
    // Polling must not change what the reader reads, only how it waits.
    ConnectedPair pair = make_pair();
    std::atomic<bool> stop{false};

    EXPECT_OK(protocol::write_frame(pair.client, testing_support::as_bytes("hello")));

    protocol::FrameReader reader{pair.server, protocol::kMaxFrameSize,
                                 protocol::ReadPolicy{/*poll_timeout_ms=*/20, &stop}};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    ASSERT_TRUE(outcome.is_ok()) << outcome.status().to_string();
    EXPECT_EQ(*outcome, protocol::FrameStatus::Complete);
    EXPECT_EQ(as_string(payload), "hello");
}

// --- Hostile input ----------------------------------------------------------

TEST(FramingTest, RefusesAnOversizedLengthPrefixWithoutAllocatingForIt) {
    // Five bytes of effort asking for a 4GB allocation. The cap has to be
    // checked before the buffer is sized, or a server is something anyone can
    // switch off from a shell prompt.
    ConnectedPair pair = make_pair();
    EXPECT_OK(pair.client.send_all(std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0x00}));

    protocol::FrameReader reader{pair.server};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    EXPECT_FALSE(outcome.is_ok());
    EXPECT_TRUE(outcome.status().is_corruption()) << outcome.status().to_string();
}

TEST(FramingTest, HonoursACustomFrameCap) {
    // Same rule at a size a test can afford to exercise directly.
    ConnectedPair pair = make_pair();
    EXPECT_OK(pair.client.send_all(framed(std::string(64, 'x'))));

    protocol::FrameReader reader{pair.server, /*max_frame=*/16};
    std::vector<std::uint8_t> payload;
    auto outcome = reader.read_frame(&payload);

    EXPECT_FALSE(outcome.is_ok());
    EXPECT_TRUE(outcome.status().is_corruption()) << outcome.status().to_string();
}

TEST(FramingTest, WriteFrameRefusesAPayloadOverTheCap) {
    // The cap binds on the way out too. Better to fail here than to send a frame
    // no conforming reader will accept.
    ConnectedPair pair = make_pair();
    const std::vector<std::uint8_t> too_big(protocol::kMaxFrameSize + 1, 0);

    EXPECT_STATUS(StatusCode::InvalidArgument, protocol::write_frame(pair.client, too_big));
}

}  // namespace
}  // namespace kvstore
