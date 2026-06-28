#include <gtest/gtest.h>
#include "../src/snapshot/Snapshot.h"
#include <cstring>

namespace {

void SetName(redscope::Snapshot& s, uint16_t idx, const char* name) {
    std::strncpy(s.moduleNames[idx], name, redscope::kModuleNameCap - 1);
    s.moduleNames[idx][redscope::kModuleNameCap - 1] = '\0';
}

void AddModule(redscope::Snapshot& s, uintptr_t base, uint32_t size, const char* name) {
    uint32_t i = s.moduleCount++;
    s.modules[i].base         = base;
    s.modules[i].size         = size;
    s.modules[i].nameIndex    = (uint16_t)i;
    s.modules[i].versionIndex = (uint16_t)i;
    SetName(s, (uint16_t)i, name);
}

}

TEST(FindModuleByPC, EmptySnapshotReturnsNull) {
    redscope::Snapshot s{};
    EXPECT_EQ(redscope::FindModuleByPC(s, 0x1000), nullptr);
    EXPECT_EQ(redscope::FindModuleByPC(s, 0), nullptr);
}

TEST(FindModuleByPC, PopulatedSnapshotFindsInRangePC) {
    redscope::Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "alpha.dll");
    AddModule(s, 0x20000000, 0x4000, "bravo.dll");
    AddModule(s, 0x30000000, 0x1000, "charlie.dll");

    auto* a = redscope::FindModuleByPC(s, 0x10000000);
    ASSERT_NE(a, nullptr);
    EXPECT_STREQ(s.moduleNames[a->nameIndex], "alpha.dll");

    auto* a_mid = redscope::FindModuleByPC(s, 0x10001FFF);
    ASSERT_NE(a_mid, nullptr);
    EXPECT_STREQ(s.moduleNames[a_mid->nameIndex], "alpha.dll");

    auto* b = redscope::FindModuleByPC(s, 0x20002000);
    ASSERT_NE(b, nullptr);
    EXPECT_STREQ(s.moduleNames[b->nameIndex], "bravo.dll");

    auto* c = redscope::FindModuleByPC(s, 0x30000500);
    ASSERT_NE(c, nullptr);
    EXPECT_STREQ(s.moduleNames[c->nameIndex], "charlie.dll");
}

TEST(FindModuleByPC, PCInGapReturnsNull) {
    redscope::Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "alpha.dll");
    AddModule(s, 0x20000000, 0x4000, "bravo.dll");

    EXPECT_EQ(redscope::FindModuleByPC(s, 0x10002000), nullptr);
    EXPECT_EQ(redscope::FindModuleByPC(s, 0x15000000), nullptr);
    EXPECT_EQ(redscope::FindModuleByPC(s, 0x1FFFFFFF), nullptr);
}

TEST(FindModuleByPC, PCPastLastModuleReturnsNull) {
    redscope::Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "alpha.dll");
    AddModule(s, 0x20000000, 0x4000, "bravo.dll");

    EXPECT_EQ(redscope::FindModuleByPC(s, 0x20004000), nullptr);
    EXPECT_EQ(redscope::FindModuleByPC(s, 0x30000000), nullptr);
    EXPECT_EQ(redscope::FindModuleByPC(s, 0xFFFFFFFFFFFFFFFFULL), nullptr);
}

TEST(FindModuleByPC, PCBeforeFirstModuleReturnsNull) {
    redscope::Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "alpha.dll");

    EXPECT_EQ(redscope::FindModuleByPC(s, 0x00000000), nullptr);
    EXPECT_EQ(redscope::FindModuleByPC(s, 0x0FFFFFFF), nullptr);
}
