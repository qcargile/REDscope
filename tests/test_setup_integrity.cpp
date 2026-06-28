#include <gtest/gtest.h>
#include "../src/snapshot/SetupIntegrity.h"
#include <string>

using redscope::snap::ModInventory;
using redscope::snap::InstalledMod;
using redscope::snap::ModType;
using redscope::snap::FrameworkPresence;
using redscope::snap::ComputeSetupIntegrity;
using redscope::snap::SetupIntegrity;

namespace {

InstalledMod Mod(const char* name, ModType type, bool enabled = true) {
    InstalledMod m;
    m.name    = name;
    m.type    = type;
    m.enabled = enabled;
    return m;
}

bool HasKind(const SetupIntegrity& si, const char* kind) {
    for (const auto& i : si.issues) {
        if (i.kind == kind) return true;
    }
    return false;
}

}

TEST(SetupIntegrity, CetModsWithoutCetFlagsMissingFramework) {
    ModInventory inv;
    inv.mods.push_back(Mod("SomeCetMod", ModType::Cet));
    FrameworkPresence fw;
    fw.cet = false;
    fw.tweakXL = true;
    auto si = ComputeSetupIntegrity(inv, fw, 0);
    EXPECT_TRUE(HasKind(si, "missing-framework"));
    EXPECT_NE(si.issues[0].detail.find("Cyber Engine Tweaks"), std::string::npos);
}

TEST(SetupIntegrity, TweakXLModsWithoutTweakXLFlagsMissingFramework) {
    ModInventory inv;
    inv.mods.push_back(Mod("SomeTweak", ModType::TweakXL));
    FrameworkPresence fw;
    fw.cet = true;
    fw.tweakXL = false;
    auto si = ComputeSetupIntegrity(inv, fw, 0);
    EXPECT_TRUE(HasKind(si, "missing-framework"));
    EXPECT_NE(si.issues[0].detail.find("TweakXL"), std::string::npos);
}

TEST(SetupIntegrity, AllFrameworksPresentNoIssues) {
    ModInventory inv;
    inv.mods.push_back(Mod("SomeCetMod", ModType::Cet));
    inv.mods.push_back(Mod("SomeTweak", ModType::TweakXL));
    inv.mods.push_back(Mod("SomeArchive", ModType::Archive));
    FrameworkPresence fw;
    fw.cet = true;
    fw.tweakXL = true;
    auto si = ComputeSetupIntegrity(inv, fw, 0);
    EXPECT_TRUE(si.issues.empty());
}

TEST(SetupIntegrity, DisabledModsDoNotTriggerFrameworkIssue) {
    ModInventory inv;
    inv.mods.push_back(Mod("SomeCetMod", ModType::Cet, false));
    FrameworkPresence fw;
    fw.cet = false;
    fw.tweakXL = false;
    auto si = ComputeSetupIntegrity(inv, fw, 0);
    EXPECT_TRUE(si.issues.empty());
}

TEST(SetupIntegrity, PluginLoadFailuresFlagged) {
    ModInventory inv;
    FrameworkPresence fw;
    fw.cet = true;
    fw.tweakXL = true;
    auto si = ComputeSetupIntegrity(inv, fw, 3);
    EXPECT_TRUE(HasKind(si, "plugin-load-failure"));
    EXPECT_NE(si.issues[0].detail.find("3"), std::string::npos);
}
