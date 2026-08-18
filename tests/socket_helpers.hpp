#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kvstore/socket.hpp"

namespace kvstore::testing_support {

// Loopback only, everywhere. Nothing in this suite binds a routable address:
// it keeps the tests off the network, and on Windows it is also what stops the
// firewall putting a dialog box in front of a test run.
inline constexpr const char* kLoopback = "127.0.0.1";

inline std::span<const std::uint8_t> as_bytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

inline std::string as_string(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A listener and the two ends of one connection through it.
//
// Needs no thread: connect() completes without accept() being called at all.
// The kernel finishes the three-way handshake on the listener's behalf and parks
// the connection in the backlog queue, so the accept() below returns one that is
// already established.
struct ConnectedPair {
    Listener listener;
    Socket client;
    Socket server;
};

inline ConnectedPair make_pair() {
    auto listener = Listener::bind(kLoopback, 0);
    EXPECT_TRUE(listener.is_ok()) << listener.status().to_string();

    auto client = Socket::connect(kLoopback, listener->port());
    EXPECT_TRUE(client.is_ok()) << client.status().to_string();

    auto server = listener->accept();
    EXPECT_TRUE(server.is_ok()) << server.status().to_string();

    return ConnectedPair{listener.take(), client.take(), server.take()};
}

}  // namespace kvstore::testing_support
