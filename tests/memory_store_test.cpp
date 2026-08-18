#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "kvstore/memory_store.hpp"
#include "kvstore/status.hpp"
#include "kvstore_contract.hpp"

namespace kvstore {
namespace {

struct MemoryStoreFactory {
    std::unique_ptr<KVStore> create() { return std::make_unique<MemoryStore>(); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(MemoryStore, KVStoreContract, MemoryStoreFactory);

TEST(StatusTest, OkHasNoMessage) {
    const Status s = Status::ok();
    EXPECT_TRUE(s.is_ok());
    EXPECT_TRUE(static_cast<bool>(s));
    EXPECT_TRUE(s.message().empty());
    EXPECT_EQ(s.to_string(), "OK");
}

TEST(StatusTest, ErrorCarriesCodeAndMessage) {
    const Status s = Status::not_found("no such key: alpha");
    EXPECT_FALSE(s.is_ok());
    EXPECT_FALSE(static_cast<bool>(s));
    EXPECT_TRUE(s.is_not_found());
    EXPECT_EQ(s.code(), StatusCode::NotFound);
    EXPECT_EQ(s.to_string(), "NotFound: no such key: alpha");
}

TEST(StatusTest, DefaultConstructedIsOk) {
    // `return {};` must mean success -- call sites rely on it.
    const Status s;
    EXPECT_TRUE(s.is_ok());
}

}  // namespace
}  // namespace kvstore
