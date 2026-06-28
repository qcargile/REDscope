#include <gtest/gtest.h>
#include "../src/snapshot/RttiSnapshot.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/report/Sections.h"
#include "../src/util/PreallocatedBuffer.h"
#include <string>
#include <vector>

namespace redscope::snap {
void TestSetCurrentSnapshot(const Snapshot* s) noexcept;
}

using redscope::snap::ClassWithScriptedFields;
using redscope::snap::RttiSnapshot;
using redscope::snap::ScriptedField;

using redscope::snap::ScriptedField;

TEST(RttiSnapshot, BuildCappedSortsByFieldCountDescending) {
    std::vector<ClassWithScriptedFields> classes;

    ClassWithScriptedFields small;
    small.name = "Small";
    small.totalFieldCount = 1;
    small.fields.push_back({"m_s", "Int32", 0x10, false, false});

    ClassWithScriptedFields large;
    large.name = "Large";
    large.totalFieldCount = 5;
    for (int i = 0; i < 3; ++i) {
        large.fields.push_back({"m_l" + std::to_string(i), "Int32",
                                (uint32_t)(0x10 + i * 4), false, false});
    }

    classes.push_back(std::move(small));
    classes.push_back(std::move(large));

    auto snap = redscope::snap::BuildCappedSnapshotForTest(std::move(classes), 100);

    ASSERT_EQ(snap.classes.size(), 2u);
    EXPECT_EQ(snap.classes[0].name, "Large");
    EXPECT_EQ(snap.classes[1].name, "Small");
    EXPECT_EQ(snap.totalScriptedFields, 4u);
    EXPECT_EQ(snap.truncatedClasses, 0u);
    EXPECT_EQ(snap.truncatedFieldsPerClass, 0u);
}

TEST(RttiSnapshot, BuildCappedTruncatesOverflowingFieldCount) {
    std::vector<ClassWithScriptedFields> classes;
    ClassWithScriptedFields c;
    c.name = "Busy";
    c.totalFieldCount = 40;
    for (size_t i = 0; i < redscope::snap::kMaxFieldsPerClass + 5; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "m_f%zu", i);
        c.fields.push_back({name, "Int32", (uint32_t)(0x10 + i * 4), false, false});
    }
    classes.push_back(std::move(c));

    auto snap = redscope::snap::BuildCappedSnapshotForTest(std::move(classes), 1);

    ASSERT_EQ(snap.classes.size(), 1u);
    EXPECT_EQ(snap.classes[0].fields.size(), redscope::snap::kMaxFieldsPerClass);
    EXPECT_EQ(snap.truncatedFieldsPerClass, 5u);
}

TEST(RttiSnapshot, BuildCappedTruncatesOverflowingClassCount) {
    std::vector<ClassWithScriptedFields> classes;
    for (size_t i = 0; i < redscope::snap::kMaxClassesWithScriptedFields + 3; ++i) {
        ClassWithScriptedFields c;
        char name[32];
        std::snprintf(name, sizeof(name), "Class_%04zu", i);
        c.name = name;
        c.totalFieldCount = (uint32_t)(i + 1);
        for (size_t j = 0; j <= i; ++j) {
            c.fields.push_back({"m_f", "Int32", (uint32_t)j, false, false});
        }
        classes.push_back(std::move(c));
    }
    auto snap = redscope::snap::BuildCappedSnapshotForTest(std::move(classes), 9999);
    EXPECT_EQ(snap.classes.size(), redscope::snap::kMaxClassesWithScriptedFields);
    EXPECT_EQ(snap.truncatedClasses, 3u);
}

TEST(RttiSnapshot, FindScriptedFieldClassHitsExactName) {
    std::vector<ClassWithScriptedFields> classes;
    ClassWithScriptedFields a; a.name = "PlayerPuppet";
    a.scriptedFieldCount = 17; a.totalFieldCount = 200;
    ClassWithScriptedFields b; b.name = "NPCPuppet";
    b.scriptedFieldCount = 4;  b.totalFieldCount = 120;
    classes.push_back(std::move(a));
    classes.push_back(std::move(b));
    auto snap = redscope::snap::BuildCappedSnapshotForTest(std::move(classes), 2);

    auto* hit = redscope::snap::FindScriptedFieldClass(snap, "PlayerPuppet");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->scriptedFieldCount, 17u);
    EXPECT_EQ(hit->name, "PlayerPuppet");
}

TEST(RttiSnapshot, FindScriptedFieldClassSkipsClassesWithZeroScriptedFields) {
    std::vector<ClassWithScriptedFields> classes;
    ClassWithScriptedFields a; a.name = "VanillaOnly";
    a.scriptedFieldCount = 0;  a.totalFieldCount = 50;
    classes.push_back(std::move(a));
    auto snap = redscope::snap::BuildCappedSnapshotForTest(std::move(classes), 1);

    auto* hit = redscope::snap::FindScriptedFieldClass(snap, "VanillaOnly");
    EXPECT_EQ(hit, nullptr);
}

TEST(RttiSnapshot, FindScriptedFieldClassReturnsNullOnMiss) {
    std::vector<ClassWithScriptedFields> classes;
    ClassWithScriptedFields a; a.name = "PlayerPuppet";
    a.scriptedFieldCount = 3;  a.totalFieldCount = 10;
    classes.push_back(std::move(a));
    auto snap = redscope::snap::BuildCappedSnapshotForTest(std::move(classes), 1);

    EXPECT_EQ(redscope::snap::FindScriptedFieldClass(snap, "Unknown"), nullptr);
    EXPECT_EQ(redscope::snap::FindScriptedFieldClass(snap, ""), nullptr);
    EXPECT_EQ(redscope::snap::FindScriptedFieldClass(snap, nullptr), nullptr);
}

TEST(RttiSnapshot, FindScriptedFieldClassHandlesEmptySnapshot) {
    RttiSnapshot empty;
    EXPECT_EQ(redscope::snap::FindScriptedFieldClass(empty, "Foo"), nullptr);
}

TEST(RttiSnapshot, CaptureStubReturnsEmptyWithoutRtti) {
    EXPECT_FALSE(redscope::snap::IsRttiReady());
    auto snap = redscope::snap::CaptureRttiSnapshot();
    EXPECT_FALSE(snap.ready);
    EXPECT_EQ(snap.totalClassesWalked, 0u);
    EXPECT_TRUE(snap.classes.empty());
}
