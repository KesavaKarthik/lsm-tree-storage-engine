#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kvstore/socket.hpp"
#include "kvstore/status.hpp"
#include "kvstore_contract.hpp"  // EXPECT_OK / EXPECT_STATUS.
#include "socket_helpers.hpp"

// Resource discipline for sockets, with no protocol involved. Everything here is
// loopback-only and binds port 0, so nothing depends on a port being free and
// nothing prompts the Windows firewall.

namespace kvstore {
namespace {

using testing_support::as_bytes;
using testing_support::as_string;
using testing_support::ConnectedPair;
using testing_support::kLoopback;
using testing_support::make_pair;

// --- Binding ----------------------------------------------------------------

TEST(ListenerTest, BindsAnEphemeralPort) {
    auto listener = Listener::bind(kLoopback, 0);
    ASSERT_TRUE(listener.is_ok()) << listener.status().to_string();

    // Port 0 means "the kernel picks"; getsockname is how we learn what it picked.
    EXPECT_NE(listener->port(), 0);
    EXPECT_TRUE(listener->is_open());
}

TEST(ListenerTest, TwoListenersGetDifferentPorts) {
    auto first = Listener::bind(kLoopback, 0);
    auto second = Listener::bind(kLoopback, 0);
    ASSERT_TRUE(first.is_ok());
    ASSERT_TRUE(second.is_ok());
    EXPECT_NE(first->port(), second->port());
}

TEST(ListenerTest, BindingAMalformedAddressFails) {
    auto listener = Listener::bind("not-an-address", 0);
    EXPECT_FALSE(listener.is_ok());
    EXPECT_TRUE(listener.status().is_io_error()) << listener.status().to_string();
}

TEST(SocketTest, ConnectingToAMalformedAddressFails) {
    auto sock = Socket::connect("999.999.999.999", 1);
    EXPECT_FALSE(sock.is_ok());
    EXPECT_TRUE(sock.status().is_io_error()) << sock.status().to_string();
}

// --- Transferring bytes -----------------------------------------------------

TEST(SocketTest, RoundTripsBytesOverLoopback) {
    ConnectedPair pair = make_pair();

    const std::string message = "hello";
    EXPECT_OK(pair.client.send_all(as_bytes(message)));

    std::vector<std::uint8_t> buf(64);
    auto got = pair.server.recv_some(buf);
    ASSERT_TRUE(got.is_ok()) << got.status().to_string();
    EXPECT_EQ(as_string(std::span{buf}.first(*got)), message);
}

TEST(SocketTest, TransfersBytesInBothDirections) {
    ConnectedPair pair = make_pair();

    EXPECT_OK(pair.client.send_all(as_bytes("ping")));
    std::vector<std::uint8_t> buf(64);
    auto got = pair.server.recv_some(buf);
    ASSERT_TRUE(got.is_ok());
    EXPECT_EQ(as_string(std::span{buf}.first(*got)), "ping");

    EXPECT_OK(pair.server.send_all(as_bytes("pong")));
    got = pair.client.recv_some(buf);
    ASSERT_TRUE(got.is_ok());
    EXPECT_EQ(as_string(std::span{buf}.first(*got)), "pong");
}

TEST(SocketTest, SendAllHandlesAPayloadLargerThanOneSend) {
    // Bigger than the kernel's send buffer will take in one call, so send_all's
    // loop actually runs more than once. Without the loop this deadlocks or
    // truncates -- the point of the test is that it does neither.
    const std::string big(1u << 20, 'x');

    ConnectedPair pair = make_pair();

    // The reader has to run concurrently: the sender blocks once the receive
    // window fills, so nothing can drain it if this thread is the only one.
    std::string received;
    std::thread reader{[&] {
        std::vector<std::uint8_t> buf(64 * 1024);
        while (received.size() < big.size()) {
            auto got = pair.server.recv_some(buf);
            if (!got.is_ok() || *got == 0) {
                break;
            }
            received.append(as_string(std::span{buf}.first(*got)));
        }
    }};

    EXPECT_OK(pair.client.send_all(as_bytes(big)));
    reader.join();

    EXPECT_EQ(received.size(), big.size());
    EXPECT_EQ(received, big);
}

TEST(SocketTest, RecvReturnsZeroWhenThePeerCloses) {
    ConnectedPair pair = make_pair();

    EXPECT_OK(pair.client.close());

    std::vector<std::uint8_t> buf(64);
    auto got = pair.server.recv_some(buf);
    ASSERT_TRUE(got.is_ok()) << got.status().to_string();

    // Zero is not an error and not a short read -- it is the peer saying it will
    // send nothing further. Only the frame reader can decide whether that is a
    // clean disconnect or a truncated message, and it needs the distinction.
    EXPECT_EQ(*got, 0u);
}

TEST(SocketTest, RecvAfterShutdownReturnsImmediately) {
    // What shutdown() actually guarantees, and all it guarantees: a recv issued
    // *after* it returns does not block.
    //
    // The tempting stronger claim -- that it also wakes a recv already blocked
    // on another thread -- is true on POSIX and false on Winsock, where the
    // blocked call sits there until TCP gives up around two minutes later. This
    // test deliberately does not make that claim, and Server does not rely on
    // it; see ServerTest.StopsPromptlyWithManyIdleConnections for the mechanism
    // that does work.
    ConnectedPair pair = make_pair();

    EXPECT_OK(pair.server.shutdown());

    std::vector<std::uint8_t> buf(64);
    auto got = pair.server.recv_some(buf);

    // POSIX ends the stream and yields 0; Winsock fails the call. Both mean the
    // same thing, and a read loop that handles only one of them spins forever on
    // the other platform.
    const bool finished = !got.is_ok() || *got == 0;
    EXPECT_TRUE(finished);
}

TEST(SocketTest, WaitReadableTimesOutOnAnIdleConnection) {
    // The portable interruptibility primitive. A reader that waits like this
    // instead of blocking in recv can be told to stop, on every platform,
    // without anything having to reach into it.
    ConnectedPair pair = make_pair();

    auto ready = pair.server.wait_readable(20);
    ASSERT_TRUE(ready.is_ok()) << ready.status().to_string();
    EXPECT_FALSE(*ready);
}

TEST(SocketTest, WaitReadableSeesPendingBytes) {
    ConnectedPair pair = make_pair();
    EXPECT_OK(pair.client.send_all(as_bytes("data")));

    auto ready = pair.server.wait_readable(1000);
    ASSERT_TRUE(ready.is_ok()) << ready.status().to_string();
    EXPECT_TRUE(*ready);
}

// --- Ownership --------------------------------------------------------------

TEST(SocketTest, MovedFromSocketNoLongerOwnsTheHandle) {
    ConnectedPair pair = make_pair();

    Socket moved{std::move(pair.client)};
    EXPECT_TRUE(moved.is_open());
    EXPECT_FALSE(pair.client.is_open());  // NOLINT(bugprone-use-after-move)

    // The moved-to socket still works, which is the proof the handle travelled
    // rather than being duplicated.
    EXPECT_OK(moved.send_all(as_bytes("still here")));
}

TEST(SocketTest, MoveAssignmentClosesWhatItReplaces) {
    ConnectedPair first = make_pair();
    ConnectedPair second = make_pair();

    const socket_t replaced = first.client.native();
    first.client = std::move(second.client);

    EXPECT_NE(first.client.native(), replaced);
    EXPECT_FALSE(second.client.is_open());  // NOLINT(bugprone-use-after-move)
}

TEST(SocketTest, CloseIsIdempotent) {
    ConnectedPair pair = make_pair();

    EXPECT_OK(pair.client.close());
    EXPECT_OK(pair.client.close());  // Closing twice is not an error.
    EXPECT_FALSE(pair.client.is_open());
}

TEST(SocketTest, OperationsOnAClosedSocketFailRatherThanUseTheHandle) {
    ConnectedPair pair = make_pair();
    EXPECT_OK(pair.client.close());

    EXPECT_STATUS(StatusCode::IOError, pair.client.send_all(as_bytes("x")));
    EXPECT_STATUS(StatusCode::IOError, pair.client.shutdown());

    std::vector<std::uint8_t> buf(4);
    auto got = pair.client.recv_some(buf);
    EXPECT_FALSE(got.is_ok());
    EXPECT_TRUE(got.status().is_io_error());
}

TEST(ListenerTest, CloseIsIdempotentAndOperationsFailAfterwards) {
    auto listener = Listener::bind(kLoopback, 0);
    ASSERT_TRUE(listener.is_ok());

    EXPECT_OK(listener->close());
    EXPECT_OK(listener->close());
    EXPECT_FALSE(listener->is_open());

    auto accepted = listener->accept();
    EXPECT_FALSE(accepted.is_ok());
    EXPECT_TRUE(accepted.status().is_io_error());
}

// --- Waiting ----------------------------------------------------------------

TEST(ListenerTest, WaitReadableTimesOutWhenNobodyConnects) {
    auto listener = Listener::bind(kLoopback, 0);
    ASSERT_TRUE(listener.is_ok());

    auto ready = listener->wait_readable(20);
    ASSERT_TRUE(ready.is_ok()) << ready.status().to_string();
    EXPECT_FALSE(*ready);  // A timeout, not an error -- the accept loop relies on it.
}

TEST(ListenerTest, WaitReadableSeesAPendingConnection) {
    auto listener = Listener::bind(kLoopback, 0);
    ASSERT_TRUE(listener.is_ok());

    auto client = Socket::connect(kLoopback, listener->port());
    ASSERT_TRUE(client.is_ok()) << client.status().to_string();

    auto ready = listener->wait_readable(1000);
    ASSERT_TRUE(ready.is_ok()) << ready.status().to_string();
    EXPECT_TRUE(*ready);

    // And the promise it makes holds: accept() does not block.
    auto server = listener->accept();
    EXPECT_TRUE(server.is_ok()) << server.status().to_string();
}

}  // namespace
}  // namespace kvstore
