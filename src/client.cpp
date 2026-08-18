#include "kvstore/client.hpp"

#include <utility>
#include <vector>

namespace kvstore {

Result<std::unique_ptr<Client>> Client::connect(const std::string& host, std::uint16_t port) {
    auto sock = Socket::connect(host, port);
    if (!sock.is_ok()) {
        return sock.status();
    }
    // `new` rather than make_unique: the constructor is private.
    return std::unique_ptr<Client>{new Client{sock.take()}};
}

Status Client::close() { return sock_.close(); }

Result<protocol::Response> Client::exchange(const protocol::Request& request) {
    KVSTORE_RETURN_IF_ERROR(protocol::write_frame(sock_, protocol::encode_request(request)));

    std::vector<std::uint8_t> payload;
    auto incoming = reader_.read_frame(&payload);
    if (!incoming.is_ok()) {
        return incoming.status();
    }
    if (*incoming == protocol::FrameStatus::PeerClosed) {
        // The server hung up rather than answering. Distinguished from a
        // protocol error because there is nothing wrong with the bytes -- there
        // simply were not any.
        return Status::io_error("server closed the connection without answering");
    }

    return protocol::decode_response(payload);
}

Status Client::put(const std::string& key, const std::string& value) {
    auto response = exchange(protocol::Request{protocol::Op::Put, key, value});
    if (!response.is_ok()) {
        return response.status();
    }
    return protocol::to_status(*response);
}

Status Client::get(const std::string& key, std::string* value) {
    auto response = exchange(protocol::Request{protocol::Op::Get, key, ""});
    if (!response.is_ok()) {
        return response.status();
    }
    if (response->code != StatusCode::Ok) {
        // Includes NotFound, which is an answer rather than a failure -- and, as
        // the contract requires, *value is left alone on any non-Ok status.
        return protocol::to_status(*response);
    }

    *value = std::move(response->body);
    return Status::ok();
}

Status Client::remove(const std::string& key) {
    auto response = exchange(protocol::Request{protocol::Op::Delete, key, ""});
    if (!response.is_ok()) {
        return response.status();
    }
    return protocol::to_status(*response);
}

}  // namespace kvstore
