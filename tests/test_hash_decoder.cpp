#include <gtest/gtest.h>
#include "../src/rtti/HashDecoder.h"
#include <cstring>
#include <string>

using redscope::rtti::DecodedHash;
using redscope::rtti::HashResolvers;
using redscope::rtti::SetHashResolvers;
using redscope::rtti::GetHashResolvers;
using redscope::rtti::TryDecodeCName;
using redscope::rtti::TryDecodeTweakDBID;

namespace {

const char* FakeCNameGet(uint64_t hash) noexcept {
    switch (hash) {
        case 0x11111111ull: return "PlayerPuppet";
        case 0x22222222ull: return "NPCPuppet";
        case 0x33333333ull: return "";
        default:            return nullptr;
    }
}

const char* FakeTdbIdLookup(uint64_t id) noexcept {
    if (id == 0x1234560CAABBCCDDull) return "gamedataItemRecord";
    if (id == 0x0008200800000BEFull) return "gamedataWeaponRecord";
    return nullptr;
}

struct ResolverFixture : public ::testing::Test {
    void SetUp() override {
        prev_ = GetHashResolvers();
        HashResolvers r;
        r.cnameGet    = &FakeCNameGet;
        r.tdbIdLookup = &FakeTdbIdLookup;
        SetHashResolvers(r);
    }
    void TearDown() override {
        SetHashResolvers(prev_);
    }
    HashResolvers prev_{};
};

}

TEST(HashDecoder, NoResolverSetReturnsNotDecoded) {
    HashResolvers empty{};
    HashResolvers prev = GetHashResolvers();
    SetHashResolvers(empty);

    auto r = TryDecodeCName(0x11111111ull);
    EXPECT_FALSE(r.decoded);
    EXPECT_EQ(r.text[0], '\0');

    SetHashResolvers(prev);
}

TEST_F(ResolverFixture, DecodesKnownCName) {
    auto r = TryDecodeCName(0x11111111ull);
    EXPECT_TRUE(r.decoded);
    EXPECT_STREQ(r.text, "PlayerPuppet");
}

TEST_F(ResolverFixture, DecodesDifferentKnownCName) {
    auto r = TryDecodeCName(0x22222222ull);
    EXPECT_TRUE(r.decoded);
    EXPECT_STREQ(r.text, "NPCPuppet");
}

TEST_F(ResolverFixture, UnknownHashNotDecoded) {
    auto r = TryDecodeCName(0xDEADBEEFull);
    EXPECT_FALSE(r.decoded);
    EXPECT_EQ(r.text[0], '\0');
}

TEST_F(ResolverFixture, EmptyPoolReturnNotDecoded) {
    auto r = TryDecodeCName(0x33333333ull);
    EXPECT_FALSE(r.decoded);
    EXPECT_EQ(r.text[0], '\0');
}

TEST_F(ResolverFixture, ZeroHashNotDecoded) {
    auto r = TryDecodeCName(0ull);
    EXPECT_FALSE(r.decoded);
}

TEST_F(ResolverFixture, SmallHashesSkippedAsLikelyScalars) {
    auto r1 = TryDecodeCName(1ull);
    auto r2 = TryDecodeCName(42ull);
    auto r3 = TryDecodeCName(0xFFFFull);
    EXPECT_FALSE(r1.decoded);
    EXPECT_FALSE(r2.decoded);
    EXPECT_FALSE(r3.decoded);
}

TEST_F(ResolverFixture, TweakDBIDDecodesKnownRecord) {
    auto r = TryDecodeTweakDBID(0x1234560CAABBCCDDull);
    EXPECT_TRUE(r.decoded);
    EXPECT_NE(std::string(r.text).find("gamedataItemRecord"), std::string::npos);
    EXPECT_NE(std::string(r.text).find("len=12"), std::string::npos);
    EXPECT_NE(std::string(r.text).find("0xAABBCCDD"), std::string::npos);
}

TEST_F(ResolverFixture, TweakDBIDDecodesSecondKnownRecord) {
    auto r = TryDecodeTweakDBID(0x0008200800000BEFull);
    EXPECT_TRUE(r.decoded);
    EXPECT_NE(std::string(r.text).find("gamedataWeaponRecord"), std::string::npos);
}

TEST_F(ResolverFixture, TweakDBIDZeroIsNotDecoded) {
    auto r = TryDecodeTweakDBID(0);
    EXPECT_FALSE(r.decoded);
}

TEST_F(ResolverFixture, TweakDBIDWithZeroHashNotDecoded) {
    auto r = TryDecodeTweakDBID(0x0000100000000000ull);
    EXPECT_FALSE(r.decoded);
}

TEST_F(ResolverFixture, TweakDBIDWithZeroLengthNotDecoded) {
    auto r = TryDecodeTweakDBID(0x00000000AABBCCDDull);
    EXPECT_FALSE(r.decoded);
}

TEST_F(ResolverFixture, TweakDBIDUnknownRecordNotDecoded) {
    auto r = TryDecodeTweakDBID(0x00001000DEADBEEFull);
    EXPECT_FALSE(r.decoded);
}

TEST_F(ResolverFixture, TweakDBIDWithNoResolverNotDecoded) {
    auto prev = GetHashResolvers();
    HashResolvers r{};
    r.cnameGet = &FakeCNameGet;
    SetHashResolvers(r);

    auto d = TryDecodeTweakDBID(0x1234560CAABBCCDDull);
    EXPECT_FALSE(d.decoded);

    SetHashResolvers(prev);
}

TEST_F(ResolverFixture, LongPoolStringTruncatedToBuffer) {
    static const char kLong[] =
        "AVeryLongNameThatExceedsTheFixedBufferCapacityForDecodedHashOutputAndShouldBeTruncatedCleanly";
    auto prev = GetHashResolvers();
    HashResolvers r{};
    r.cnameGet = [](uint64_t h) noexcept -> const char* {
        return (h == 0xAAAAAAAAull) ? kLong : nullptr;
    };
    SetHashResolvers(r);

    auto d = TryDecodeCName(0xAAAAAAAAull);
    EXPECT_TRUE(d.decoded);
    EXPECT_EQ(d.text[sizeof(d.text) - 1], '\0');
    EXPECT_GT(std::strlen(d.text), 0u);

    SetHashResolvers(prev);
}
