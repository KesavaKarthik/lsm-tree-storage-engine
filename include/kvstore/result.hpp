#pragma once

#include <cassert>
#include <optional>
#include <utility>

#include "kvstore/status.hpp"

namespace kvstore {

// A value or a Status, never both. C++23 has std::expected; this is the
// C++20-sized version of it.
//
// Needed because some operations have to return data *and* be able to fail --
// "append these bytes and tell me the offset" can fail with IOError, and
// there's no offset value that could stand in for "it didn't happen".
template <typename T>
class [[nodiscard]] Result {
public:
    // Implicit on purpose: `return offset;` in a Result<uint64_t> function.
    Result(T value) : value_(std::move(value)) {}

    // Implicit on purpose: `return Status::io_error(...);`
    Result(Status status) : status_(std::move(status)) {
        assert(!status_.is_ok() && "a failed Result needs a failed Status");
    }

    [[nodiscard]] bool is_ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return is_ok(); }

    // Ok when is_ok(); the failure otherwise.
    [[nodiscard]] Status status() const { return is_ok() ? Status::ok() : status_; }

    // Precondition: is_ok().
    [[nodiscard]] const T& operator*() const& { return *value_; }
    [[nodiscard]] T& operator*() & { return *value_; }
    [[nodiscard]] const T* operator->() const { return &*value_; }
    [[nodiscard]] T* operator->() { return &*value_; }

    // Move the value out. Precondition: is_ok().
    [[nodiscard]] T take() { return std::move(*value_); }

private:
    std::optional<T> value_;
    Status status_;
};

}  // namespace kvstore
