#include <gtest/gtest.h>
#include "../src/report/Fingerprint.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/snapshot/InstalledMods.h"
#include "../src/util/PreallocatedBuffer.h"
#include <windows.h>
#include <cstring>
#include <string>

using redscope::Snapshot;
using redscope::ModuleKind;
using redscope::kModuleNameCap;
using redscope::snap::InstalledMod;
using redscope::snap::ModType;
using redscope::report::CrashFingerprint;
using redscope::report::ComputeFingerprintCore;
using redscope::report::ComputeModSetHash;
using redscope::report::ComputeFingerprint;
using redscope::report::EmitCrashId;

namespace {

void AddModule(Snapshot& s, uintptr_t base, uint32_t size, const char* name, ModuleKind kind) {
    uint32_t i = s.moduleCount++;
    s.modules[i].base         = base;
    s.modules[i].size         = size;
    s.modules[i].nameIndex    = (uint16_t)i;
    s.modules[i].versionIndex = (uint16_t)i;
    s.modules[i].kind         = kind;
    std::strncpy(s.moduleNames[i], name, kModuleNameCap - 1);
}

InstalledMod Mod(const char* name, const char* version, bool enabled = true) {
    InstalledMod m;
    m.name    = name;
    m.version = version;
    m.enabled = enabled;
    m.type    = ModType::Red4ext;
    return m;
}

const char* const kTail2[] = {"FuncInner", "FuncOuter"};

}

TEST(Fingerprint, CoreIsDeterministic) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "archivexl.dll", 0x1234, nullptr, kTail2, 2, 99, false);
    auto b = ComputeFingerprintCore(0xC0000005u, "2.21", "archivexl.dll", 0x1234, nullptr, kTail2, 2, 99, false);
    EXPECT_EQ(a.primary, b.primary);
    EXPECT_EQ(a.loose, b.loose);
    EXPECT_STREQ(a.primaryId, b.primaryId);
    EXPECT_STREQ(a.looseId, b.looseId);
}

TEST(Fingerprint, ModSetNotInPrimaryOrLoose) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x500, nullptr, kTail2, 2, 111, false);
    auto b = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x500, nullptr, kTail2, 2, 222, false);
    EXPECT_EQ(a.primary, b.primary);
    EXPECT_EQ(a.loose, b.loose);
    EXPECT_NE(a.modSet, b.modSet);
}

TEST(Fingerprint, PrimaryDiffersFromLooseWithTail) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x500, nullptr, kTail2, 2, 0, false);
    EXPECT_NE(a.primary, a.loose);
}

TEST(Fingerprint, PrimaryEqualsLooseWithoutTail) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x500, nullptr, nullptr, 0, 0, false);
    EXPECT_EQ(a.primary, a.loose);
}

TEST(Fingerprint, DifferentRvaDiffersLoose) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x500, nullptr, nullptr, 0, 0, false);
    auto b = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x900, nullptr, nullptr, 0, 0, false);
    EXPECT_NE(a.loose, b.loose);
}

TEST(Fingerprint, DifferentBuildDiffersFingerprint) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.13", "cyberpunk2077.exe", 0x500, nullptr, nullptr, 0, 0, false);
    auto b = ComputeFingerprintCore(0xC0000005u, "2.14", "cyberpunk2077.exe", 0x500, nullptr, nullptr, 0, 0, false);
    EXPECT_NE(a.loose, b.loose);
}

TEST(Fingerprint, SymbolPreferredOverRva) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x500, "gameDelaySystem::Tick", nullptr, 0, 0, false);
    auto b = ComputeFingerprintCore(0xC0000005u, "2.21", "cyberpunk2077.exe", 0x900, "gameDelaySystem::Tick", nullptr, 0, 0, false);
    EXPECT_EQ(a.loose, b.loose);
}

TEST(Fingerprint, IdsAreEightCharCrockford) {
    auto a = ComputeFingerprintCore(0xC0000005u, "2.21", "x.dll", 0x1, nullptr, nullptr, 0, 0, false);
    EXPECT_EQ(std::strlen(a.primaryId), 8u);
    EXPECT_EQ(std::strlen(a.looseId), 8u);
    const std::string alpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (const char* p = a.primaryId; *p; ++p) {
        EXPECT_NE(alpha.find(*p), std::string::npos);
    }
}

TEST(Fingerprint, HasModuleFlag) {
    auto withMod = ComputeFingerprintCore(0xC0000005u, "2.21", "x.dll", 0x1, nullptr, nullptr, 0, 0, false);
    auto noMod   = ComputeFingerprintCore(0xC0000005u, "2.21", nullptr, 0, nullptr, nullptr, 0, 0, false);
    EXPECT_TRUE(withMod.hasModule);
    EXPECT_FALSE(noMod.hasModule);
}

TEST(ModSetHash, OrderIndependent) {
    Snapshot s1{};
    s1.inventory.mods.push_back(Mod("Alpha", "1.0"));
    s1.inventory.mods.push_back(Mod("Beta", "2.0"));
    Snapshot s2{};
    s2.inventory.mods.push_back(Mod("Beta", "2.0"));
    s2.inventory.mods.push_back(Mod("Alpha", "1.0"));
    EXPECT_EQ(ComputeModSetHash(&s1), ComputeModSetHash(&s2));
}

TEST(ModSetHash, DisabledAndEmptySkipped) {
    Snapshot base{};
    base.inventory.mods.push_back(Mod("Alpha", "1.0"));
    uint64_t h0 = ComputeModSetHash(&base);

    Snapshot withDisabled{};
    withDisabled.inventory.mods.push_back(Mod("Alpha", "1.0"));
    withDisabled.inventory.mods.push_back(Mod("Beta", "2.0", false));
    EXPECT_EQ(ComputeModSetHash(&withDisabled), h0);

    Snapshot withEmpty{};
    withEmpty.inventory.mods.push_back(Mod("Alpha", "1.0"));
    withEmpty.inventory.mods.push_back(Mod("", "9.9"));
    EXPECT_EQ(ComputeModSetHash(&withEmpty), h0);
}

TEST(ModSetHash, DifferentVersionDiffersHash) {
    Snapshot s1{};
    s1.inventory.mods.push_back(Mod("Alpha", "1.0"));
    Snapshot s2{};
    s2.inventory.mods.push_back(Mod("Alpha", "1.1"));
    EXPECT_NE(ComputeModSetHash(&s1), ComputeModSetHash(&s2));
}

TEST(ModSetHash, NullSnapshotZero) {
    EXPECT_EQ(ComputeModSetHash(nullptr), 0ull);
}

TEST(Fingerprint, FaultSiteUnreliableClasses) {
    auto heap = ComputeFingerprint(0xC0000374u, 0xDEAD, nullptr, nullptr);
    auto gs   = ComputeFingerprint(0xC0000409u, 0xDEAD, nullptr, nullptr);
    auto so   = ComputeFingerprint(static_cast<uint32_t>(EXCEPTION_STACK_OVERFLOW), 0xDEAD, nullptr, nullptr);
    auto av   = ComputeFingerprint(0xC0000005u, 0xDEAD, nullptr, nullptr);
    EXPECT_TRUE(heap.faultSiteUnreliable);
    EXPECT_TRUE(gs.faultSiteUnreliable);
    EXPECT_TRUE(so.faultSiteUnreliable);
    EXPECT_FALSE(av.faultSiteUnreliable);
}

TEST(Fingerprint, GathererModuleAndRva) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);
    std::strncpy(s.cp2077BuildString, "2.21", sizeof(s.cp2077BuildString) - 1);
    auto a = ComputeFingerprint(0xC0000005u, 0x140000500ull, &s, nullptr);
    auto b = ComputeFingerprint(0xC0000005u, 0x140000900ull, &s, nullptr);
    EXPECT_TRUE(a.hasModule);
    EXPECT_NE(a.loose, b.loose);
}

TEST(EmitCrashId, EmitsIdAndGroupingNote) {
    CrashFingerprint fp{};
    std::strncpy(fp.primaryId, "ABCD2345", sizeof(fp.primaryId) - 1);
    std::strncpy(fp.looseId, "WXYZ6789", sizeof(fp.looseId) - 1);
    fp.faultSiteUnreliable = true;
    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    EmitCrashId(buf, fp);
    std::string out(buf.Data(), buf.Size());
    EXPECT_NE(out.find("CRASH ID: ABCD2345"), std::string::npos);
    EXPECT_NE(out.find("loose WXYZ6789"), std::string::npos);
    EXPECT_NE(out.find("fault-site class"), std::string::npos);
}

TEST(EmitCrashId, NoGroupingNoteWhenReliable) {
    CrashFingerprint fp{};
    std::strncpy(fp.primaryId, "ABCD2345", sizeof(fp.primaryId) - 1);
    std::strncpy(fp.looseId, "WXYZ6789", sizeof(fp.looseId) - 1);
    fp.faultSiteUnreliable = false;
    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    EmitCrashId(buf, fp);
    std::string out(buf.Data(), buf.Size());
    EXPECT_NE(out.find("CRASH ID: ABCD2345"), std::string::npos);
    EXPECT_EQ(out.find("fault-site class"), std::string::npos);
}

TEST(Fingerprint, ModFrameGroupsGameUnsymbolicatedCrashes) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);
    std::strncpy(s.cp2077BuildString, "2.21", sizeof(s.cp2077BuildString) - 1);
    const uintptr_t faultRip = 0x140000000ull + 0x191408Cull;  // game, unsymbolicated
    const char* mod = "ArchiveXL.dll";
    const uint64_t rva = 0xE6E56;

    auto a = ComputeFingerprint(0xC0000005u, faultRip, &s, nullptr, mod, rva);
    auto b = ComputeFingerprint(0xC0000005u, faultRip, &s, nullptr, mod, rva);
    EXPECT_STREQ(a.looseId,   b.looseId);    // same bug + same mod frame -> same bucket
    EXPECT_STREQ(a.primaryId, b.primaryId);

    auto noMod = ComputeFingerprint(0xC0000005u, faultRip, &s, nullptr, nullptr, 0);
    EXPECT_STRNE(a.looseId, noMod.looseId);  // the mod frame actually participates in the id
}
