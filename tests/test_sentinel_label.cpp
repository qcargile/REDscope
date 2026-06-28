#include <gtest/gtest.h>
#include "../src/rtti/PointerType.h"
#include <cstring>

using redscope::rtti::MatchSentinelLabel;

TEST(SentinelLabel, RecognisesInvalidI64) {
    char buf[64] = {};
    EXPECT_TRUE(MatchSentinelLabel(0xFFFFFFFFFFFFFFFFull, buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "INVALID"), nullptr);
    EXPECT_NE(std::strstr(buf, "i64"), nullptr);
}

TEST(SentinelLabel, RecognisesInvalidI32InHighZeros) {
    char buf[64] = {};
    EXPECT_TRUE(MatchSentinelLabel(0x00000000FFFFFFFFull, buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "INVALID"), nullptr);
    EXPECT_NE(std::strstr(buf, "i32"), nullptr);
}

TEST(SentinelLabel, RecognisesInt64Max) {
    char buf[64] = {};
    EXPECT_TRUE(MatchSentinelLabel(0x7FFFFFFFFFFFFFFFull, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "INT64_MAX");
}

TEST(SentinelLabel, RecognisesInt64Min) {
    char buf[64] = {};
    EXPECT_TRUE(MatchSentinelLabel(0x8000000000000000ull, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "INT64_MIN");
}

TEST(SentinelLabel, RecognisesInt32MaxInHighZeros) {
    char buf[64] = {};
    EXPECT_TRUE(MatchSentinelLabel(0x000000007FFFFFFFull, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "INT32_MAX");
}

TEST(SentinelLabel, RecognisesNegativeTwoSentinels) {
    char buf[64] = {};
    EXPECT_TRUE(MatchSentinelLabel(0xFFFFFFFFFFFFFFFEull, buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "-2"), nullptr);
    std::memset(buf, 0, sizeof(buf));
    EXPECT_TRUE(MatchSentinelLabel(0x00000000FFFFFFFEull, buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "-2"), nullptr);
}

TEST(SentinelLabel, DoesNotMatchOrdinaryValues) {
    char buf[64] = {};
    EXPECT_FALSE(MatchSentinelLabel(0, buf, sizeof(buf)));
    EXPECT_FALSE(MatchSentinelLabel(1, buf, sizeof(buf)));
    EXPECT_FALSE(MatchSentinelLabel(0x1234567890ABCDEFull, buf, sizeof(buf)));
    EXPECT_FALSE(MatchSentinelLabel(0x00007FF663B5D62Dull, buf, sizeof(buf)));
    EXPECT_FALSE(MatchSentinelLabel(0xDD11C5620ull, buf, sizeof(buf)));
}

TEST(SentinelLabel, Int32MaxWithNonZeroHighIsNotSentinel) {
    char buf[64] = {};
    EXPECT_FALSE(MatchSentinelLabel(0x0000000180000000ull, buf, sizeof(buf)));
    EXPECT_FALSE(MatchSentinelLabel(0x00000001FFFFFFFFull, buf, sizeof(buf)));
}

TEST(SentinelLabel, HandlesTooSmallBuffer) {
    char buf[1] = {};
    EXPECT_FALSE(MatchSentinelLabel(0xFFFFFFFFFFFFFFFFull, buf, sizeof(buf)));
}
