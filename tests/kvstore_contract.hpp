#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "kvstore/kvstore.hpp"
#include "kvstore/status.hpp"

// The KVStore contract, written once and run against every implementation.
//
// Phase 0 predicted this: the cases were already written against a
// KVStore pointer, so making them shared meant lifting them into a typed test
// suite rather than rewriting them. Each implementation supplies a Factory
// (see the INSTANTIATE_TYPED_TEST_SUITE_P calls in the *_test.cpp files) whose
// create() returns a fresh, empty store.
//
// The value of this file is that Bitcask must satisfy exactly what MemoryStore
// satisfies -- no "well, the disk one behaves a bit differently".

namespace kvstore {

#define EXPECT_STATUS(expected_code, expr)                                     \
    do {                                                                       \
        const ::kvstore::Status _s = (expr);                                   \
        EXPECT_EQ(_s.code(), (expected_code))                                  \
            << "expected " << ::kvstore::Status::code_to_string(expected_code) \
            << ", got " << _s.to_string();                                     \
    } while (0)

#define EXPECT_OK(expr) EXPECT_STATUS(::kvstore::StatusCode::Ok, (expr))

template <typename Factory>
class KVStoreContract : public ::testing::Test {
protected:
    void SetUp() override { store_ = factory_.create(); }

    Factory factory_;  // Owns whatever the store needs (a temp dir, say).
    std::unique_ptr<KVStore> store_;
};

TYPED_TEST_SUITE_P(KVStoreContract);

TYPED_TEST_P(KVStoreContract, PutThenGetReturnsTheValue) {
    EXPECT_OK(this->store_->put("alpha", "one"));

    std::string value;
    EXPECT_OK(this->store_->get("alpha", &value));
    EXPECT_EQ(value, "one");
}

TYPED_TEST_P(KVStoreContract, GetOnMissingKeyReturnsNotFound) {
    std::string value = "sentinel";
    EXPECT_STATUS(StatusCode::NotFound, this->store_->get("absent", &value));
    // Nothing asserted about `value`: unspecified on a non-Ok status.
}

TYPED_TEST_P(KVStoreContract, PutOverwritesExistingValue) {
    EXPECT_OK(this->store_->put("alpha", "first"));
    EXPECT_OK(this->store_->put("alpha", "second"));

    std::string value;
    EXPECT_OK(this->store_->get("alpha", &value));
    EXPECT_EQ(value, "second");
}

TYPED_TEST_P(KVStoreContract, RemoveThenGetReturnsNotFound) {
    EXPECT_OK(this->store_->put("alpha", "one"));
    EXPECT_OK(this->store_->remove("alpha"));

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, this->store_->get("alpha", &value));
}

TYPED_TEST_P(KVStoreContract, RemoveOnMissingKeyIsOk) {
    EXPECT_OK(this->store_->remove("never-existed"));

    EXPECT_OK(this->store_->put("alpha", "one"));
    EXPECT_OK(this->store_->remove("alpha"));
    EXPECT_OK(this->store_->remove("alpha"));
}

TYPED_TEST_P(KVStoreContract, KeysAreIndependent) {
    EXPECT_OK(this->store_->put("alpha", "one"));
    EXPECT_OK(this->store_->put("beta", "two"));
    EXPECT_OK(this->store_->remove("alpha"));

    std::string value;
    EXPECT_STATUS(StatusCode::NotFound, this->store_->get("alpha", &value));
    EXPECT_OK(this->store_->get("beta", &value));
    EXPECT_EQ(value, "two");
}

TYPED_TEST_P(KVStoreContract, EmptyValueRoundTrips) {
    EXPECT_OK(this->store_->put("alpha", ""));

    std::string value = "sentinel";
    EXPECT_OK(this->store_->get("alpha", &value));
    EXPECT_EQ(value, "");
}

TYPED_TEST_P(KVStoreContract, ValuesAreBinarySafe) {
    const std::string key("k\0ey", 4);
    const std::string blob("a\0b\0c", 5);
    EXPECT_OK(this->store_->put(key, blob));

    std::string value;
    EXPECT_OK(this->store_->get(key, &value));
    EXPECT_EQ(value, blob);
    EXPECT_EQ(value.size(), 5u);
}

TYPED_TEST_P(KVStoreContract, EmptyKeyIsRejected) {
    EXPECT_STATUS(StatusCode::InvalidArgument, this->store_->put("", "value"));
}

TYPED_TEST_P(KVStoreContract, ManyKeys) {
    // Enough records that the log holds more than a handful, so an offset bug
    // has room to show up.
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        EXPECT_OK(this->store_->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    for (int i = 0; i < kCount; ++i) {
        std::string value;
        EXPECT_OK(this->store_->get("key" + std::to_string(i), &value));
        EXPECT_EQ(value, "value" + std::to_string(i));
    }
}

REGISTER_TYPED_TEST_SUITE_P(KVStoreContract,
                            PutThenGetReturnsTheValue,
                            GetOnMissingKeyReturnsNotFound,
                            PutOverwritesExistingValue,
                            RemoveThenGetReturnsNotFound,
                            RemoveOnMissingKeyIsOk,
                            KeysAreIndependent,
                            EmptyValueRoundTrips,
                            ValuesAreBinarySafe,
                            EmptyKeyIsRejected,
                            ManyKeys);

}  // namespace kvstore
