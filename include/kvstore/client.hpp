#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kvstore/kvstore.hpp"
#include "kvstore/protocol.hpp"
#include "kvstore/result.hpp"
#include "kvstore/socket.hpp"
#include "kvstore/status.hpp"

namespace kvstore {

// A KVStore that happens to live on the other end of a socket.
//
// **Implementing the interface rather than offering some other API is the whole
// idea.** Phase 0 wrote the contract down so that the implementation could be
// swapped without callers noticing; a network hop is just another swap. Two
// things fall out of that for free:
//
//   - kv-cli is a thin main() over this, and does not know the wire format.
//   - the KVStoreContract suite -- the same ten cases that Bitcask and
//     MemoryStore satisfy -- can be instantiated against a Client talking to a
//     real server over a real socket. Every guarantee the contract makes is then
//     re-checked end to end, and the awkward one (values containing NULs) turns
//     out to be exactly the case that proves the framing carries bytes rather
//     than strings.
//
// One connection, used synchronously: a request goes out, an answer comes back,
// and nothing else may be in flight. Not thread-safe, and it does not need to be
// -- the server is what handles many clients, and a caller wanting concurrency
// opens more connections.
class Client final : public KVStore {
public:
    [[nodiscard]] static Result<std::unique_ptr<Client>> connect(const std::string& host,
                                                                 std::uint16_t port);

    ~Client() override = default;

    Status put(const std::string& key, const std::string& value) override;
    Status get(const std::string& key, std::string* value) override;
    Status remove(const std::string& key) override;

    // Hangs up. The server sees zero bytes at a frame boundary, which is the
    // clean end of a conversation rather than anything to report.
    [[nodiscard]] Status close();

private:
    explicit Client(Socket sock) noexcept : sock_(std::move(sock)), reader_(sock_) {}

    // Sends one request and waits for its answer.
    [[nodiscard]] Result<protocol::Response> exchange(const protocol::Request& request);

    // Declaration order is load-bearing: reader_ holds a reference to sock_, so
    // sock_ must be initialised first. It is also why this class is neither
    // movable nor copyable -- moving it would leave reader_ pointing at the
    // socket the moved-from object used to have. KVStore deletes the copy
    // operations, which suppresses the implicit moves too.
    Socket sock_;
    protocol::FrameReader reader_;
};

}  // namespace kvstore
