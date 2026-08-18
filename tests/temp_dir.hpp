#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace kvstore::testing_support {

// A directory that deletes itself. Tests that leave databases behind eventually
// interfere with each other, and a test that fails mid-way must still clean up
// -- which is the destructor's job, not the test body's.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("kvstore_test_" + std::to_string(counter.fetch_add(1)) + "_" +
                 std::to_string(static_cast<unsigned long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);  // Best effort; never throw here.
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace kvstore::testing_support
