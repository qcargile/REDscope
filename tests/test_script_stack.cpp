#include <gtest/gtest.h>
#include "../src/hooks/ScriptStack.h"

using redscope::scriptstack::Current;
using redscope::scriptstack::Frame;
using redscope::scriptstack::kMaxDepth;
using redscope::scriptstack::Pop;
using redscope::scriptstack::Push;
using redscope::scriptstack::ResetForTest;

namespace {

Frame MakeFrame(const char* name, int64_t ns = 0) {
    Frame f;
    f.fullName  = name;
    f.enteredNs = ns;
    return f;
}

class ScriptStackTest : public ::testing::Test {
protected:
    void SetUp() override { ResetForTest(); }
    void TearDown() override { ResetForTest(); }
};

}

TEST_F(ScriptStackTest, EmptyInitially) {
    EXPECT_EQ(Current().depth,        0u);
    EXPECT_EQ(Current().overflow,     0u);
    EXPECT_EQ(Current().maxDepthSeen, 0u);
}

TEST_F(ScriptStackTest, PushAndPopBalances) {
    Push(MakeFrame("a", 100));
    Push(MakeFrame("b", 200));
    EXPECT_EQ(Current().depth, 2u);
    Pop();
    EXPECT_EQ(Current().depth, 1u);
    Pop();
    EXPECT_EQ(Current().depth, 0u);
}

TEST_F(ScriptStackTest, PopOnEmptyIsNoop) {
    Pop();
    Pop();
    Pop();
    EXPECT_EQ(Current().depth,    0u);
    EXPECT_EQ(Current().overflow, 0u);
}

TEST_F(ScriptStackTest, PushPreservesFullName) {
    Push(MakeFrame("Item.HasTag;String", 123));
    Push(MakeFrame("StatusEffectHelper::ApplyStatusEffect", 456));

    const auto& s = Current();
    ASSERT_EQ(s.depth, 2u);
    EXPECT_STREQ(s.frames[0].fullName, "Item.HasTag;String");
    EXPECT_EQ   (s.frames[0].enteredNs, 123);
    EXPECT_STREQ(s.frames[1].fullName, "StatusEffectHelper::ApplyStatusEffect");
    EXPECT_EQ   (s.frames[1].enteredNs, 456);
}

TEST_F(ScriptStackTest, MaxDepthSeenTracksHighWatermark) {
    Push(MakeFrame("a"));
    Push(MakeFrame("b"));
    Push(MakeFrame("c"));
    EXPECT_EQ(Current().maxDepthSeen, 3u);
    Pop();
    Pop();
    EXPECT_EQ(Current().maxDepthSeen, 3u);
    Push(MakeFrame("d"));
    EXPECT_EQ(Current().maxDepthSeen, 3u);
    Push(MakeFrame("e"));
    Push(MakeFrame("f"));
    EXPECT_EQ(Current().maxDepthSeen, 4u);
}

TEST_F(ScriptStackTest, OverflowIncrementsCounterNotFrames) {
    for (size_t i = 0; i < kMaxDepth; ++i) {
        Push(MakeFrame("within_cap"));
    }
    EXPECT_EQ(Current().depth,    kMaxDepth);
    EXPECT_EQ(Current().overflow, 0u);

    for (size_t i = 0; i < 5; ++i) {
        Push(MakeFrame("beyond_cap"));
    }
    EXPECT_EQ(Current().depth,    kMaxDepth);
    EXPECT_EQ(Current().overflow, 5u);
}

TEST_F(ScriptStackTest, PopDrainsOverflowBeforeFrames) {
    for (size_t i = 0; i < kMaxDepth + 3; ++i) {
        Push(MakeFrame("x"));
    }
    ASSERT_EQ(Current().depth,    kMaxDepth);
    ASSERT_EQ(Current().overflow, 3u);

    Pop();
    EXPECT_EQ(Current().depth,    kMaxDepth);
    EXPECT_EQ(Current().overflow, 2u);

    Pop();
    Pop();
    EXPECT_EQ(Current().depth,    kMaxDepth);
    EXPECT_EQ(Current().overflow, 0u);

    Pop();
    EXPECT_EQ(Current().depth,    kMaxDepth - 1);
    EXPECT_EQ(Current().overflow, 0u);
}

TEST_F(ScriptStackTest, UnbalancedPopsDoNotUnderflow) {
    Push(MakeFrame("a"));
    Pop();
    Pop();
    Pop();
    Push(MakeFrame("b"));
    EXPECT_EQ(Current().depth, 1u);
}

TEST_F(ScriptStackTest, ResetForTestClearsEverything) {
    Push(MakeFrame("a"));
    Push(MakeFrame("b"));
    ResetForTest();
    EXPECT_EQ(Current().depth,        0u);
    EXPECT_EQ(Current().overflow,     0u);
    EXPECT_EQ(Current().maxDepthSeen, 0u);
}
