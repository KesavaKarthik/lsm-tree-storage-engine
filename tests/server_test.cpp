#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/bitcask.hpp"
#include "kvstore/client.hpp"
#include "kvstore/locked_store.hpp"
#include "kvstore/memory_store.hpp"
#include "kvstore/protocol.hpp"
#include "kvstore/server.hpp"
#include "kvstore/socket.hpp"
#include "kvstore/status.hpp"
#include "kvstore_contract.hpp"
#include "socket_helpers.hpp"
#include "store_helpers.hpp"
#include "temp_dir.hpp"

namespace kvstore {
namespace {

using testing_support::kLoopback;
using testing_support::TempDir;

// A whole stack in one object: a temp directory, a Bitcask in it, the lock, and
// a server in front of them.
//
// **Member order is load-bearing on Windows.** Destruction runs in reverse
// declaration order, so the server stops (joining every thread) before the
// engine closes its files, which happens before the directory tries to delete
// itself. Windows will not remove a directory holding an open handle, so getting
// this backwards does not race -- it fails, every time, in the destructor.
struct ServerFixture {
    ServerFixture() {
        engine = testing_support::open_or_fail(dir, BitcaskOptions{SyncMode::Never});
        EXPECT_NE(engine, nullptr);
        locked = std::make_unique<LockedStore>(*engine);

        auto started = Server::start(*locked);
        EXPECT_TRUE(started.is_ok()) << started.status().to_string();
        server = started.is_ok() ? started.take() : nullptr;
    }

    [[nodiscard]] std::uint16_t port() const { return server->port(); }

    [[nodiscard]] std::unique_ptr<Client> client() const {
        auto connected = Client::connect(kLoopback, port());
        EXPECT_TRUE(connected.is_ok()) << connected.status().to_string();
        return connected.is_ok() ? connected.take() : nullptr;
    }

    // A bare socket, for the cases that need to control the bytes themselves.
    [[nodiscard]] Socket raw_client() const {
        auto sock = Socket::connect(kLoopback, port());
        EXPECT_TRUE(sock.is_ok()) << sock.status().to_string();
        return sock.take();
    }

    TempDir dir;
    std::unique_ptr<Bitcask> engine;
    std::unique_ptr<LockedStore> locked;
    std::unique_ptr<Server> server;
};

// --- The whole contract, over a real socket ---------------------------------

// The ten cases every KVStore implementation satisfies, run against a client
// talking to a server over loopback. Nothing new is asserted and that is the
// value: whatever the interface promised in Phase 0, a network hop must not
// quietly change -- including that values may contain NULs, which is the case
// that would fail instantly if the wire carried C strings.
struct NetworkFactory {
    std::unique_ptr<KVStore> create() {
        fixture = std::make_unique<ServerFixture>();
        auto connected = Client::connect(kLoopback, fixture->port());
        EXPECT_TRUE(connected.is_ok()) << connected.status().to_string();
        return connected.is_ok() ? std::unique_ptr<KVStore>{connected.take()} : nullptr;
    }

    // Declared first so it outlives the client the fixture's store_ holds.
    std::unique_ptr<ServerFixture> fixture;
};

INSTANTIATE_TYPED_TEST_SUITE_P(Network, KVStoreContract, NetworkFactory);

// --- Round trips ------------------------------------------------------------

TEST(ServerTest, ServesPutGetAndDeleteOverASocket) {
    ServerFixture fixture;
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);

    EXPECT_OK(client->put("user:1", "alice"));

    std::string value;
    EXPECT_OK(client->get("user:1", &value));
    EXPECT_EQ(value, "alice");

    EXPECT_OK(client->remove("user:1"));
    EXPECT_STATUS(StatusCode::NotFound, client->get("user:1", &value));
}

TEST(ServerTest, BindsAnEphemeralPortAndReportsIt) {
    ServerFixture fixture;
    EXPECT_NE(fixture.port(), 0);
}

TEST(ServerTest, WritesReachTheEngineBehindIt) {
    // The server is a transport, so what it puts had better be in the database.
    ServerFixture fixture;
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);

    EXPECT_OK(client->put("alpha", "one"));
    EXPECT_OK(client->put("beta", "two"));

    EXPECT_EQ(fixture.engine->key_count(), 2u);
}

TEST(ServerTest, ManyRequestsOnOneConnection) {
    // Pipelined down a single connection, which is where the frame reader's
    // leftover handling gets exercised for real: replies and requests arrive in
    // whatever groupings the kernel chooses.
    ServerFixture fixture;
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);

    constexpr int kCount = 500;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(client->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    for (int i = 0; i < kCount; ++i) {
        std::string value;
        EXPECT_OK(client->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

TEST(ServerTest, CarriesLargeValues) {
    ServerFixture fixture;
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);

    const std::string big(1u << 20, 'v');
    EXPECT_OK(client->put("big", big));

    std::string value;
    EXPECT_OK(client->get("big", &value));
    EXPECT_EQ(value.size(), big.size());
    EXPECT_EQ(value, big);
}

// --- Decoupled from the storage engine --------------------------------------

TEST(ServerTest, ServesAMemoryStoreWithNoDiskInvolved) {
    // The decoupling, proved by construction rather than asserted in a comment:
    // this test never names Bitcask, never touches a directory, and the server
    // cannot tell the difference.
    MemoryStore inner;
    LockedStore locked{inner};

    auto started = Server::start(locked);
    ASSERT_TRUE(started.is_ok()) << started.status().to_string();
    auto server = started.take();

    auto connected = Client::connect(kLoopback, server->port());
    ASSERT_TRUE(connected.is_ok());
    auto client = connected.take();

    EXPECT_OK(client->put("alpha", "one"));
    std::string value;
    EXPECT_OK(client->get("alpha", &value));
    EXPECT_EQ(value, "one");
    EXPECT_EQ(inner.size(), 1u);
}

// --- Many clients -----------------------------------------------------------

TEST(ServerTest, ServesManyConnectionsAtOnce) {
    ServerFixture fixture;

    constexpr int kClients = 8;
    constexpr int kPerClient = 100;

    std::vector<std::thread> threads;
    threads.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&fixture, c] {
            auto connected = Client::connect(kLoopback, fixture.port());
            ASSERT_TRUE(connected.is_ok()) << connected.status().to_string();
            auto client = connected.take();

            for (int i = 0; i < kPerClient; ++i) {
                const std::string key = std::to_string(c) + ":" + std::to_string(i);
                EXPECT_OK(client->put(key, "value" + key));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(fixture.engine->key_count(), static_cast<std::size_t>(kClients * kPerClient));
    EXPECT_GE(fixture.server->connections_accepted(), static_cast<std::uint64_t>(kClients));

    // Everything each client wrote is readable, so no request was lost and no
    // two connections' writes were confused for one another.
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);
    for (int c = 0; c < kClients; ++c) {
        for (int i = 0; i < kPerClient; i += 17) {
            const std::string key = std::to_string(c) + ":" + std::to_string(i);
            std::string value;
            EXPECT_OK(client->get(key, &value));
            EXPECT_EQ(value, "value" + key);
        }
    }
}

// --- Framing, end to end ----------------------------------------------------

TEST(ServerTest, AnswersARequestSplitAcrossThreeSends) {
    // The partial-read proof at the level that matters: not the reader in
    // isolation, but the server actually answering a request that never arrived
    // as one piece.
    ServerFixture fixture;
    Socket sock = fixture.raw_client();

    const std::vector<std::uint8_t> payload =
        protocol::encode_request(protocol::Request{protocol::Op::Put, "split", "value"});

    std::vector<std::uint8_t> frame{
        static_cast<std::uint8_t>(payload.size() >> 24),
        static_cast<std::uint8_t>(payload.size() >> 16),
        static_cast<std::uint8_t>(payload.size() >> 8),
        static_cast<std::uint8_t>(payload.size()),
    };
    frame.insert(frame.end(), payload.begin(), payload.end());

    // One byte of the length prefix, then most of the rest, then the tail. A
    // server that treated one recv as one message would decode garbage from the
    // first chunk and never recover.
    EXPECT_OK(sock.send_all(std::span{frame}.first(1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_OK(sock.send_all(std::span{frame}.subspan(1, frame.size() - 3)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_OK(sock.send_all(std::span{frame}.last(2)));

    protocol::FrameReader reader{sock};
    std::vector<std::uint8_t> answer;
    auto incoming = reader.read_frame(&answer);
    ASSERT_TRUE(incoming.is_ok()) << incoming.status().to_string();
    ASSERT_EQ(*incoming, protocol::FrameStatus::Complete);

    auto response = protocol::decode_response(answer);
    ASSERT_TRUE(response.is_ok());
    EXPECT_EQ(response->code, StatusCode::Ok);

    std::string value;
    EXPECT_OK(fixture.engine->get("split", &value));
    EXPECT_EQ(value, "value");
}

TEST(ServerTest, AnswersTwoRequestsDeliveredInOneSend) {
    ServerFixture fixture;
    Socket sock = fixture.raw_client();

    std::vector<std::uint8_t> both;
    for (const char* key : {"first", "second"}) {
        const std::vector<std::uint8_t> payload =
            protocol::encode_request(protocol::Request{protocol::Op::Put, key, "v"});
        both.push_back(static_cast<std::uint8_t>(payload.size() >> 24));
        both.push_back(static_cast<std::uint8_t>(payload.size() >> 16));
        both.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        both.push_back(static_cast<std::uint8_t>(payload.size()));
        both.insert(both.end(), payload.begin(), payload.end());
    }

    EXPECT_OK(sock.send_all(both));  // Two messages, one send.

    protocol::FrameReader reader{sock};
    std::vector<std::uint8_t> answer;
    for (int i = 0; i < 2; ++i) {
        auto incoming = reader.read_frame(&answer);
        ASSERT_TRUE(incoming.is_ok()) << incoming.status().to_string();
        EXPECT_EQ(*incoming, protocol::FrameStatus::Complete);
    }

    // Both landed: the server did not drop the second request with the tail of
    // the buffer the first one arrived in.
    EXPECT_EQ(fixture.engine->key_count(), 2u);
}

// --- Bad input --------------------------------------------------------------

TEST(ServerTest, AnswersAMalformedRequestAndKeepsTheConnection) {
    // A nonsense *message* is answerable: the frame boundaries are still known,
    // so the server says InvalidArgument and carries on. This is the distinction
    // between a broken message and a broken stream.
    ServerFixture fixture;
    Socket sock = fixture.raw_client();

    const std::vector<std::uint8_t> nonsense{0x7f, 0x00, 0x00, 0x00, 0x00};  // Unknown op.
    EXPECT_OK(protocol::write_frame(sock, nonsense));

    protocol::FrameReader reader{sock};
    std::vector<std::uint8_t> answer;
    ASSERT_TRUE(reader.read_frame(&answer).is_ok());

    auto response = protocol::decode_response(answer);
    ASSERT_TRUE(response.is_ok());
    EXPECT_EQ(response->code, StatusCode::InvalidArgument);

    // Still usable afterwards -- the connection was not collateral damage.
    const std::vector<std::uint8_t> good =
        protocol::encode_request(protocol::Request{protocol::Op::Put, "after", "v"});
    EXPECT_OK(protocol::write_frame(sock, good));
    ASSERT_TRUE(reader.read_frame(&answer).is_ok());

    response = protocol::decode_response(answer);
    ASSERT_TRUE(response.is_ok());
    EXPECT_EQ(response->code, StatusCode::Ok);
}

TEST(ServerTest, DropsAConnectionThatAnnouncesAnImpossibleFrame) {
    // A broken *stream*: after a length nobody will honour, there is no way to
    // find where the next message starts. The connection goes, the server stays.
    ServerFixture fixture;
    Socket sock = fixture.raw_client();

    EXPECT_OK(sock.send_all(std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}));

    protocol::FrameReader reader{sock};
    std::vector<std::uint8_t> answer;
    auto incoming = reader.read_frame(&answer);

    // Either the server closed without answering, or the read failed outright.
    const bool connection_ended =
        !incoming.is_ok() || *incoming == protocol::FrameStatus::PeerClosed;
    EXPECT_TRUE(connection_ended);

    // And the server is still serving everyone else.
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);
    EXPECT_OK(client->put("still", "alive"));
}

TEST(ServerTest, SurvivesAClientThatHangsUpMidFrame) {
    ServerFixture fixture;
    {
        Socket sock = fixture.raw_client();
        // A prefix promising 100 bytes, then two, then gone.
        EXPECT_OK(sock.send_all(std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x64, 'a', 'b'}));
    }

    auto client = fixture.client();
    ASSERT_NE(client, nullptr);
    EXPECT_OK(client->put("after", "the truncated one"));
}

TEST(ServerTest, SurvivesAClientThatConnectsAndSaysNothing) {
    ServerFixture fixture;
    { Socket sock = fixture.raw_client(); }  // Connect, close immediately.

    auto client = fixture.client();
    ASSERT_NE(client, nullptr);
    EXPECT_OK(client->put("after", "the silent one"));
}

// --- Shutdown ---------------------------------------------------------------

TEST(ServerTest, StopIsIdempotent) {
    ServerFixture fixture;
    fixture.server->stop();
    fixture.server->stop();  // The destructor will call it a third time.
}

TEST(ServerTest, StopsWithAClientStillConnected) {
    // This connection's thread is waiting for a request that never comes. If
    // stop() could not reach it, the test would hang rather than fail.
    ServerFixture fixture;
    auto client = fixture.client();
    ASSERT_NE(client, nullptr);
    EXPECT_OK(client->put("alpha", "one"));

    fixture.server->stop();
    SUCCEED();  // Reaching here at all is the assertion.
}

TEST(ServerTest, StopsPromptlyWithManyIdleConnections) {
    // Idle connections are the hard case, and the one that caught a real bug:
    // every serving thread is parked waiting for a request that will never come,
    // and stop() has to get all of them back. Asserting on the *duration* is the
    // point -- the broken version passed this test's correctness checks and took
    // two minutes to do it, because it relied on shutdown() waking a blocked
    // recv, which Winsock does not do.
    ServerFixture fixture;

    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 8; ++i) {
        auto connected = Client::connect(kLoopback, fixture.port());
        ASSERT_TRUE(connected.is_ok());
        clients.push_back(connected.take());
    }

    const auto start = std::chrono::steady_clock::now();
    fixture.server->stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // A few poll intervals is expected; anything near a TCP timeout means the
    // threads were not woken but merely outlived.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000);
}

TEST(ServerTest, ReleasesConnectionsAsClientsLeave) {
    // Connections are reaped rather than accumulating for the server's lifetime.
    ServerFixture fixture;

    for (int i = 0; i < 20; ++i) {
        auto client = fixture.client();
        ASSERT_NE(client, nullptr);
        EXPECT_OK(client->put("key" + std::to_string(i), "v"));
        EXPECT_OK(client->close());
    }

    EXPECT_EQ(fixture.server->connections_accepted(), 20u);

    // The accept loop reaps on each pass, so give it a couple of poll timeouts
    // to notice. This is about not growing without bound, not about promptness.
    for (int i = 0; i < 20 && fixture.server->active_connections() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(fixture.server->active_connections(), 0u);
}

TEST(ServerTest, StartAndStopRepeatedlyLeaksNothing) {
    // Under LeakSanitizer this is the test that matters: twenty servers, each
    // with a live connection, all torn down. Any thread that outlives its Server
    // or any socket that outlives its Connection shows up here.
    for (int i = 0; i < 20; ++i) {
        MemoryStore inner;
        LockedStore locked{inner};

        auto started = Server::start(locked);
        ASSERT_TRUE(started.is_ok()) << started.status().to_string();
        auto server = started.take();

        auto connected = Client::connect(kLoopback, server->port());
        ASSERT_TRUE(connected.is_ok());
        EXPECT_OK(connected.take()->put("alpha", "one"));
    }
}

}  // namespace
}  // namespace kvstore
