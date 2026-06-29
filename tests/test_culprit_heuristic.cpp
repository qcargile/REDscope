#include <gtest/gtest.h>
#include "../src/report/CulpritHeuristic.h"
#include "../src/rtti/ObjectsInFlight.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/util/PreallocatedBuffer.h"
#include <windows.h>
#include <cstring>
#include <string>

using redscope::ModuleKind;
using redscope::Snapshot;
using redscope::kModuleNameCap;
using redscope::kModuleVersionCap;
using redscope::report::ComputeVerdict;
using redscope::report::CulpritCategory;
using redscope::report::CulpritConfidence;
using redscope::report::CulpritVerdict;
using redscope::report::EmitCulpritLine;

namespace {

void SetStr(char* dst, size_t cap, const char* src) {
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void AddModule(Snapshot& s, uintptr_t base, uint32_t size,
               const char* name, ModuleKind kind, const char* version = "") {
    uint32_t i = s.moduleCount++;
    s.modules[i].base         = base;
    s.modules[i].size         = size;
    s.modules[i].nameIndex    = (uint16_t)i;
    s.modules[i].versionIndex = (uint16_t)i;
    s.modules[i].kind         = kind;
    SetStr(s.moduleNames[i],    kModuleNameCap,    name);
    SetStr(s.moduleVersions[i], kModuleVersionCap, version);
}

void AddPluginMeta(Snapshot& s, const char* dllLower,
                   const char* name, const char* version, const char* authors) {
    redscope::snap::PluginInfo pi;
    pi.name        = name;
    pi.version     = version;
    pi.authors     = authors;
    pi.dllBasename = dllLower;
    s.pluginMeta.byDll.emplace(std::string(dllLower), std::move(pi));
}

}

TEST(CulpritHeuristic, NullSnapshotReturnsUnknown) {
    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0xDEADBEEF, nullptr);
    EXPECT_EQ(v.category,   CulpritCategory::Unknown);
    EXPECT_EQ(v.confidence, CulpritConfidence::Unknown);
    EXPECT_NE(std::string(v.text).find("snapshot unavailable"), std::string::npos);
}

TEST(CulpritHeuristic, StackOverflowOverridesRip) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "ArchiveXL.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_STACK_OVERFLOW, 0x10000042, &s);
    EXPECT_EQ(v.category,   CulpritCategory::StackOverflow);
    EXPECT_EQ(v.confidence, CulpritConfidence::High);
    EXPECT_NE(std::string(v.text).find("recursion"), std::string::npos);
}

TEST(CulpritHeuristic, HeapCorruptionOverridesRip) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "TweakXL.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(0xC0000374, 0x10000042, &s);
    EXPECT_EQ(v.category,   CulpritCategory::Memory);
    EXPECT_EQ(v.confidence, CulpritConfidence::High);
    EXPECT_NE(std::string(v.text).find("heap"), std::string::npos);
}

TEST(CulpritHeuristic, StackBufferOverrunIsMemory) {
    auto v = ComputeVerdict(0xC0000409, 0x0, nullptr);
    EXPECT_EQ(v.category, CulpritCategory::Memory);
    EXPECT_EQ(v.confidence, CulpritConfidence::High);
    EXPECT_NE(std::string(v.text).find("GS check"), std::string::npos);
}

TEST(CulpritHeuristic, InPageErrorIsMemoryMedium) {
    auto v = ComputeVerdict(EXCEPTION_IN_PAGE_ERROR, 0x0, nullptr);
    EXPECT_EQ(v.category,   CulpritCategory::Memory);
    EXPECT_EQ(v.confidence, CulpritConfidence::Medium);
}

TEST(CulpritHeuristic, RipInModHighConfidence) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "ArchiveXL.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000100, &s);
    EXPECT_EQ(v.category,   CulpritCategory::Mod);
    EXPECT_EQ(v.confidence, CulpritConfidence::High);
    std::string text(v.text);
    EXPECT_NE(text.find("ArchiveXL.dll"), std::string::npos);
    EXPECT_NE(text.find("0x100"),         std::string::npos);
}

TEST(CulpritHeuristic, RipInModIncludesVersionAndAuthor) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "ArchiveXL.dll", ModuleKind::Mod);
    AddPluginMeta(s, "archivexl.dll", "ArchiveXL", "1.26.2", "psiberx");

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000100, &s);
    std::string text(v.text);
    EXPECT_NE(text.find("1.26.2"),  std::string::npos);
    EXPECT_NE(text.find("psiberx"), std::string::npos);
}

TEST(CulpritHeuristic, RipInModWithVersionOnlyOmitsAuthor) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "somemod.dll", ModuleKind::Mod);
    AddPluginMeta(s, "somemod.dll", "SomeMod", "0.3.0", "");

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000042, &s);
    std::string text(v.text);
    EXPECT_NE(text.find("0.3.0"), std::string::npos);
    EXPECT_EQ(text.find(" by "),  std::string::npos);
}

TEST(CulpritHeuristic, RipInCetIsHostFrameworkLowConfidence) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x10000, "cyber_engine_tweaks.asi", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x1000AC37, &s);
    EXPECT_EQ(v.confidence, CulpritConfidence::Low);
    std::string text(v.text);
    EXPECT_NE(text.find("cyber_engine_tweaks.asi"), std::string::npos);
    EXPECT_NE(text.find("framework/host module"),   std::string::npos);
    EXPECT_NE(text.find("LAST ACTIVE"),             std::string::npos);
}

TEST(CulpritHeuristic, RipInRed4extIsHostFramework) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "RED4ext.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000100, &s);
    EXPECT_EQ(v.confidence, CulpritConfidence::Low);
    EXPECT_NE(std::string(v.text).find("framework/host"), std::string::npos);
}

TEST(CulpritHeuristic, RipInSccIsHostFramework) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "scc.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000100, &s);
    EXPECT_EQ(v.confidence, CulpritConfidence::Low);
    EXPECT_NE(std::string(v.text).find("framework/host"), std::string::npos);
}

TEST(CulpritHeuristic, HostFrameworkMatchIsCaseInsensitive) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "Cyber_Engine_Tweaks.ASI", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000100, &s);
    EXPECT_EQ(v.confidence, CulpritConfidence::Low);
}

TEST(CulpritHeuristic, FrameworkPluginStaysHighCulprit) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "Codeware.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x10000100, &s);
    EXPECT_EQ(v.confidence, CulpritConfidence::High);
    EXPECT_EQ(std::string(v.text).find("framework/host"), std::string::npos);
}

TEST(CulpritHeuristic, RipInRedscopeIsSelfCrash) {
    Snapshot s{};
    AddModule(s, 0x20000000, 0x8000, "REDscope.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x20001AD9, &s);
    EXPECT_EQ(v.category,   CulpritCategory::REDscope);
    EXPECT_EQ(v.confidence, CulpritConfidence::High);
    std::string text(v.text);
    EXPECT_NE(text.find("REDscope.dll"), std::string::npos);
    EXPECT_NE(text.find("self-test"),    std::string::npos);
}

TEST(CulpritHeuristic, RipInRedscopeIsCaseInsensitive) {
    Snapshot s{};
    AddModule(s, 0x20000000, 0x8000, "redscope.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x20000042, &s);
    EXPECT_EQ(v.category, CulpritCategory::REDscope);
}

TEST(CulpritHeuristic, RipInGameNoSymbol) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x140001234ull, &s);
    EXPECT_EQ(v.category,   CulpritCategory::Game);
    EXPECT_EQ(v.confidence, CulpritConfidence::Medium);
    std::string text(v.text);
    EXPECT_NE(text.find("Cyberpunk2077.exe"), std::string::npos);
    EXPECT_NE(text.find("unsymbolicated"),    std::string::npos);
}

TEST(CulpritHeuristic, RipInGameWithSymbolIncludesName) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x140001234ull, &s,
                            "gameDelaySystem::DelayEvent");
    std::string text(v.text);
    EXPECT_NE(text.find("gameDelaySystem::DelayEvent"), std::string::npos);
    EXPECT_NE(text.find("Cyberpunk2077.exe!"),          std::string::npos);
}

TEST(CulpritHeuristic, GameFaultWithModOnStackNamesMod) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x140001234ull, &s, nullptr, nullptr,
                            "ArchiveXL.dll", 0xE6E56);
    EXPECT_EQ(v.category,   CulpritCategory::Mod);
    EXPECT_EQ(v.confidence, CulpritConfidence::Medium);
    std::string text(v.text);
    EXPECT_NE(text.find("ArchiveXL.dll"),          std::string::npos);
    EXPECT_NE(text.find("on the crashing stack"),  std::string::npos);
    EXPECT_NE(text.find("Cyberpunk2077.exe"),      std::string::npos);
}

TEST(CulpritHeuristic, SystemFaultWithModOnStackNamesMod) {
    Snapshot s{};
    AddModule(s, 0x7FFD00000000ull, 0x200000, "ntdll.dll", ModuleKind::System);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x7FFD00001234ull, &s, nullptr, nullptr,
                            "ArchiveXL.dll", 0x1A2B);
    EXPECT_EQ(v.category,   CulpritCategory::Mod);
    EXPECT_EQ(v.confidence, CulpritConfidence::Medium);
    std::string text(v.text);
    EXPECT_NE(text.find("ArchiveXL.dll"),         std::string::npos);
    EXPECT_NE(text.find("on the crashing stack"), std::string::npos);
}

TEST(CulpritHeuristic, RipInSystemIsLowConfidence) {
    Snapshot s{};
    AddModule(s, 0x7FFD00000000ull, 0x200000, "ntdll.dll", ModuleKind::System);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x7FFD00001234ull, &s);
    EXPECT_EQ(v.category,   CulpritCategory::System);
    EXPECT_EQ(v.confidence, CulpritConfidence::Low);
    std::string text(v.text);
    EXPECT_NE(text.find("ntdll.dll"), std::string::npos);
    EXPECT_NE(text.find("(system)"), std::string::npos);
}

TEST(CulpritHeuristic, RipNotInAnyModuleIsUnknown) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "one.dll", ModuleKind::Mod);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x99999999ull, &s);
    EXPECT_EQ(v.category,   CulpritCategory::Unknown);
    EXPECT_EQ(v.confidence, CulpritConfidence::Unknown);
}

TEST(CulpritHeuristic, GameRowAppendsObjectsInFlight) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);

    redscope::rtti::InFlightSet inFlight;
    inFlight.count = 1;
    std::strncpy(inFlight.items[0].reg, "RCX", sizeof(inFlight.items[0].reg) - 1);
    std::strncpy(inFlight.items[0].className, "gamedataItem_Record",
                 sizeof(inFlight.items[0].className) - 1);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x140001234ull, &s, nullptr, &inFlight);
    EXPECT_EQ(v.category, CulpritCategory::Game);
    std::string text(v.text);
    EXPECT_NE(text.find("objects in registers"),  std::string::npos);
    EXPECT_NE(text.find("gamedataItem_Record"),   std::string::npos);
}

TEST(CulpritHeuristic, NullInFlightLeavesGameRowUnchanged) {
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x140001234ull, &s, nullptr, nullptr);
    std::string text(v.text);
    EXPECT_EQ(text.find("objects in registers"), std::string::npos);
}

TEST(CulpritHeuristic, NoModuleRowAppendsObjectsInFlight) {
    Snapshot s{};
    AddModule(s, 0x10000000, 0x2000, "one.dll", ModuleKind::Mod);

    redscope::rtti::InFlightSet inFlight;
    inFlight.count = 1;
    std::strncpy(inFlight.items[0].reg, "RDX", sizeof(inFlight.items[0].reg) - 1);
    std::strncpy(inFlight.items[0].className, "inkVideoWidget",
                 sizeof(inFlight.items[0].className) - 1);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x99999999ull, &s, nullptr, &inFlight);
    EXPECT_EQ(v.category, CulpritCategory::Unknown);
    std::string text(v.text);
    EXPECT_NE(text.find("does not lie inside any loaded module"), std::string::npos);
    EXPECT_NE(text.find("inkVideoWidget"), std::string::npos);
}

TEST(CulpritHeuristic, RipInUnknownKindModule) {
    Snapshot s{};
    AddModule(s, 0x30000000, 0x2000, "mystery.dll", ModuleKind::Unknown);

    auto v = ComputeVerdict(EXCEPTION_ACCESS_VIOLATION, 0x30000042, &s);
    EXPECT_EQ(v.category,   CulpritCategory::Unknown);
    EXPECT_EQ(v.confidence, CulpritConfidence::Low);
    std::string text(v.text);
    EXPECT_NE(text.find("mystery.dll"), std::string::npos);
}

TEST(EmitCulpritLine, KnownVerdictIncludesCategoryAndConfidence) {
    CulpritVerdict v{};
    v.category   = CulpritCategory::Mod;
    v.confidence = CulpritConfidence::High;
    std::strncpy(v.text, "ArchiveXL.dll+0x42 (v1.26.2 by psiberx)", sizeof(v.text) - 1);

    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    EmitCulpritLine(buf, v);

    std::string out(buf.Data(), buf.Size());
    EXPECT_NE(out.find("LIKELY CULPRIT: mod (HIGH)"), std::string::npos);
    EXPECT_NE(out.find("ArchiveXL.dll+0x42"),         std::string::npos);
    EXPECT_NE(out.find("psiberx"),                    std::string::npos);
    EXPECT_EQ(out.substr(out.size() - 2), std::string("\n\n"));
}

TEST(EmitCulpritLine, UnknownVerdictOmitsConfidence) {
    CulpritVerdict v{};
    v.category   = CulpritCategory::Unknown;
    v.confidence = CulpritConfidence::Unknown;
    std::strncpy(v.text, "snapshot unavailable at crash time",
                 sizeof(v.text) - 1);

    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    EmitCulpritLine(buf, v);

    std::string out(buf.Data(), buf.Size());
    EXPECT_NE(out.find("LIKELY CULPRIT: unknown -"), std::string::npos);
    EXPECT_EQ(out.find("(?)"),    std::string::npos);
    EXPECT_EQ(out.find("(HIGH)"), std::string::npos);
}
