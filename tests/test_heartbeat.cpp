#include <gtest/gtest.h>
#include "../src/hooks/ScriptStack.h"

using redscope::scriptstack::HeartbeatSlot;
using redscope::scriptstack::kMaxHeartbeatThreads;
using redscope::scriptstack::GetHeartbeatSlots;
using redscope::scriptstack::TestWriteHeartbeat;
using redscope::scriptstack::ResetHeartbeatsForTest;

struct HeartbeatFixture : public ::testing::Test {
    void SetUp()    override { ResetHeartbeatsForTest(); }
    void TearDown() override { ResetHeartbeatsForTest(); }
};

TEST_F(HeartbeatFixture, ResetYieldsEmptySlots) {
    const HeartbeatSlot* slots = nullptr;
    size_t count = 0;
    GetHeartbeatSlots(&slots, &count);
    EXPECT_EQ(count, kMaxHeartbeatThreads);
    ASSERT_NE(slots, nullptr);
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(slots[i].threadId, 0u);
        EXPECT_EQ(slots[i].lastFullName, nullptr);
        EXPECT_EQ(slots[i].lastEnteredNs, 0);
    }
}

TEST_F(HeartbeatFixture, WriteAndReadBackSingleSlot) {
    TestWriteHeartbeat(0, 12345, "PlayerPuppet.OnGameAttached", 1'000'000'000);
    const HeartbeatSlot* slots = nullptr;
    size_t count = 0;
    GetHeartbeatSlots(&slots, &count);
    EXPECT_EQ(slots[0].threadId, 12345u);
    EXPECT_STREQ(slots[0].lastFullName, "PlayerPuppet.OnGameAttached");
    EXPECT_EQ(slots[0].lastEnteredNs, 1'000'000'000);
    EXPECT_EQ(slots[1].threadId, 0u);
}

TEST_F(HeartbeatFixture, MultipleSlotsIndependent) {
    TestWriteHeartbeat(0, 100, "Main.Tick",          1'000'000'000);
    TestWriteHeartbeat(1, 200, "JobBuilder.Execute", 2'000'000'000);
    TestWriteHeartbeat(2, 300, "Render.Frame",       3'000'000'000);

    const HeartbeatSlot* slots = nullptr;
    size_t count = 0;
    GetHeartbeatSlots(&slots, &count);

    EXPECT_EQ(slots[0].threadId, 100u);
    EXPECT_STREQ(slots[0].lastFullName, "Main.Tick");
    EXPECT_EQ(slots[1].threadId, 200u);
    EXPECT_STREQ(slots[1].lastFullName, "JobBuilder.Execute");
    EXPECT_EQ(slots[2].threadId, 300u);
    EXPECT_STREQ(slots[2].lastFullName, "Render.Frame");
    EXPECT_EQ(slots[3].threadId, 0u);
}

TEST_F(HeartbeatFixture, OutOfRangeSlotWriteIgnored) {
    TestWriteHeartbeat(kMaxHeartbeatThreads, 999, "ShouldBeIgnored", 42);
    TestWriteHeartbeat(kMaxHeartbeatThreads + 5, 999, "AlsoIgnored", 42);
    const HeartbeatSlot* slots = nullptr;
    size_t count = 0;
    GetHeartbeatSlots(&slots, &count);
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(slots[i].threadId, 0u) << "slot " << i;
    }
}

TEST_F(HeartbeatFixture, SlotCountMatchesConstant) {
    size_t count = 0;
    GetHeartbeatSlots(nullptr, &count);
    EXPECT_EQ(count, kMaxHeartbeatThreads);
}

TEST_F(HeartbeatFixture, NullOutPointersAreSafe) {
    GetHeartbeatSlots(nullptr, nullptr);
    SUCCEED();
}
