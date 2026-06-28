#include <gtest/gtest.h>
#include "../src/rtti/HandleDetect.h"

using redscope::rtti::LooksLikeRefBlock;
using redscope::rtti::kMaxPlausibleRefs;

TEST(HandleDetect, SingleOwnerLooksRight) {
    EXPECT_TRUE(LooksLikeRefBlock(1u, 1u));
}

TEST(HandleDetect, SharedOwnershipLooksRight) {
    EXPECT_TRUE(LooksLikeRefBlock(4u, 7u));
}

TEST(HandleDetect, ManyWeakWatchersStillAccepted) {
    EXPECT_TRUE(LooksLikeRefBlock(2u, 128u));
}

TEST(HandleDetect, ZeroStrongRejected) {
    EXPECT_FALSE(LooksLikeRefBlock(0u, 1u));
    EXPECT_FALSE(LooksLikeRefBlock(0u, 0u));
}

TEST(HandleDetect, WeakSmallerThanStrongRejected) {
    EXPECT_FALSE(LooksLikeRefBlock(5u, 2u));
    EXPECT_FALSE(LooksLikeRefBlock(1u, 0u));
}

TEST(HandleDetect, HugeCountsRejected) {
    EXPECT_FALSE(LooksLikeRefBlock(kMaxPlausibleRefs, kMaxPlausibleRefs));
    EXPECT_FALSE(LooksLikeRefBlock(1u, kMaxPlausibleRefs));
    EXPECT_FALSE(LooksLikeRefBlock(0xDEADBEEFu, 0xDEADBEEFu));
}

TEST(HandleDetect, MaxMinusOneStillAccepted) {
    EXPECT_TRUE(LooksLikeRefBlock(kMaxPlausibleRefs - 1, kMaxPlausibleRefs - 1));
}
