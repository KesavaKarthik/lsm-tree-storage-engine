#pragma once

#include <string>
#include <string_view>

namespace kvstore {

// Errors are returned, not thrown. In a storage engine failure is routine
// (missing key, full disk, truncated log), and a returned value is visible at
// the call site, works across an -fno-exceptions boundary, and doesn't require
// every function in between to be exception-safe. LevelDB/RocksDB do the same.

enum class StatusCode {
    Ok,
    NotFound,        // Key absent. An answer, not a failure.
    IOError,         // read/write/fsync/open failed.
    Corruption,      // Bytes on disk aren't what we wrote.
    InvalidArgument, // Caller broke the contract.
};

// [[nodiscard]] on the type: ignoring any Status is a warning, forever.
class [[nodiscard]] Status {
public:
    Status() noexcept = default;  // Default is Ok, so `return {};` means success.

    // Factory is ok(), predicate is is_ok(): C++ won't let a static and a
    // non-static member share a name and signature.
    static Status ok() { return Status{}; }

    static Status not_found(std::string_view msg) {
        return Status{StatusCode::NotFound, msg};
    }
    static Status io_error(std::string_view msg) {
        return Status{StatusCode::IOError, msg};
    }
    static Status corruption(std::string_view msg) {
        return Status{StatusCode::Corruption, msg};
    }
    static Status invalid_argument(std::string_view msg) {
        return Status{StatusCode::InvalidArgument, msg};
    }

    [[nodiscard]] bool is_ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] bool is_not_found() const noexcept { return code_ == StatusCode::NotFound; }
    [[nodiscard]] bool is_io_error() const noexcept { return code_ == StatusCode::IOError; }
    [[nodiscard]] bool is_corruption() const noexcept { return code_ == StatusCode::Corruption; }
    [[nodiscard]] bool is_invalid_argument() const noexcept {
        return code_ == StatusCode::InvalidArgument;
    }

    // explicit, so a Status can't decay to bool where we didn't mean it to.
    explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] StatusCode code() const noexcept { return code_; }

    // Empty when is_ok().
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // "OK", or "NotFound: no such key: foo".
    [[nodiscard]] std::string to_string() const {
        std::string out{code_to_string(code_)};
        if (!message_.empty()) {
            out += ": ";
            out += message_;
        }
        return out;
    }

    [[nodiscard]] static const char* code_to_string(StatusCode code) noexcept {
        switch (code) {
            case StatusCode::Ok:              return "OK";
            case StatusCode::NotFound:        return "NotFound";
            case StatusCode::IOError:         return "IOError";
            case StatusCode::Corruption:      return "Corruption";
            case StatusCode::InvalidArgument: return "InvalidArgument";
        }
        return "Unknown";
    }

private:
    Status(StatusCode code, std::string_view msg) : code_(code), message_(msg) {}

    StatusCode code_ = StatusCode::Ok;

    // Costs an allocation on failure only; success stores an empty string (SSO).
    // LevelDB packs both into one char* to make success a null pointer -- worth
    // doing if Status ever shows up in a profile.
    std::string message_;
};

// Early-return helper: KVSTORE_RETURN_IF_ERROR(log_.append(record));
#define KVSTORE_RETURN_IF_ERROR(expr)               \
    do {                                            \
        ::kvstore::Status _kvstore_status = (expr); \
        if (!_kvstore_status.is_ok()) {             \
            return _kvstore_status;                 \
        }                                           \
    } while (0)

}  // namespace kvstore
