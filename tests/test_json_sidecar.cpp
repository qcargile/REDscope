#include <gtest/gtest.h>
#include "../src/report/JsonSidecar.h"
#include "../src/report/Fingerprint.h"
#include "../src/report/CrashClass.h"
#include "../src/rtti/ObjectsInFlight.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/snapshot/SessionHistory.h"
#include "../src/snapshot/InstalledMods.h"
#include "../src/util/PreallocatedBuffer.h"
#include <windows.h>
#include <chrono>
#include <cstring>
#include <string>

using redscope::Snapshot;
using redscope::ModuleKind;
using redscope::kModuleNameCap;
using redscope::snap::InstalledMod;
using redscope::snap::ModType;
using redscope::report::CrashFingerprint;
using redscope::report::SetLastFingerprint;
using redscope::report::BuildJsonSidecar;

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

std::string Build(const EXCEPTION_POINTERS* ep, const Snapshot* s) {
    redscope::PreallocatedBuffer buf;
    buf.Reserve(1 << 16);
    BuildJsonSidecar(buf, ep, s, std::chrono::system_clock::now());
    return std::string(buf.Data(), buf.Size());
}

}

TEST(JsonSidecar, ContainsFingerprintAndException) {
    CrashFingerprint fp{};
    std::strncpy(fp.primaryId, "ABCD2345", sizeof(fp.primaryId) - 1);
    std::strncpy(fp.looseId, "WXYZ6789", sizeof(fp.looseId) - 1);
    fp.primary = 0x1122334455667788ull;
    fp.hasModule = true;
    SetLastFingerprint(fp);

    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);
    std::strncpy(s.cp2077BuildString, "2.21", sizeof(s.cp2077BuildString) - 1);

    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    rec.ExceptionAddress = (PVOID)0x140000500ull;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;

    std::string j = Build(&ep, &s);
    EXPECT_NE(j.find("\"crashId\": \"ABCD2345\""), std::string::npos);
    EXPECT_NE(j.find("\"primary\": \"1122334455667788\""), std::string::npos);
    EXPECT_NE(j.find("\"code\": \"0xC0000005\""), std::string::npos);
    EXPECT_NE(j.find("Cyberpunk2077.exe"), std::string::npos);
    EXPECT_NE(j.find("\"rva\": \"0x500\""), std::string::npos);
    EXPECT_NE(j.find("\"buildId\": \"2.21\""), std::string::npos);
    EXPECT_NE(j.find("\"resourceAtFault\""), std::string::npos);
    EXPECT_NE(j.find("\"stackObjects\""), std::string::npos);
}

TEST(JsonSidecar, InstalledModsEmitNameVersionEnabled) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    s.inventory.mods.push_back(Mod("ArchiveXL", "1.26.2"));
    s.inventory.mods.push_back(Mod("Paused", "0.1", false));
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    std::string j = Build(&ep, &s);
    EXPECT_NE(j.find("\"ArchiveXL\""), std::string::npos);
    EXPECT_NE(j.find("\"1.26.2\""), std::string::npos);
    EXPECT_NE(j.find("\"Paused\""), std::string::npos);
    EXPECT_NE(j.find("\"enabled\": false"), std::string::npos);
}

TEST(JsonSidecar, NoFilesystemPathsOrUsername) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);
    s.inventory.mods.push_back(Mod("SomeMod", "1.0"));
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    rec.ExceptionAddress = (PVOID)0x140000500ull;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    std::string j = Build(&ep, &s);
    EXPECT_EQ(j.find("C:\\"), std::string::npos);
    EXPECT_EQ(j.find(":\\Users"), std::string::npos);
}

TEST(JsonSidecar, EscapesQuotesAndBackslashes) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    s.inventory.mods.push_back(Mod("Wei\"rd\\Mod", "1.0"));
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    std::string j = Build(&ep, &s);
    EXPECT_NE(j.find("Wei\\\"rd\\\\Mod"), std::string::npos);
}

TEST(JsonSidecar, StackModulesFromStackMemory) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);

    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);
    AddModule(s, 0x180000000ull, 0x00100000, "ModX.dll",          ModuleKind::Mod);
    AddModule(s, 0x190000000ull, 0x00100000, "kernelbase.dll",    ModuleKind::System);

    uint64_t fakeStack[600] = {};
    fakeStack[1]  = 0x140000500ull;
    fakeStack[3]  = 0x180000040ull;
    fakeStack[5]  = 0x180000200ull;
    fakeStack[7]  = 0x190000010ull;
    fakeStack[9]  = 0x0ull;
    fakeStack[11] = 0xDEADBEEFull;

    CONTEXT ctx{};
    ctx.Rsp = reinterpret_cast<DWORD64>(&fakeStack[0]);

    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    rec.ExceptionAddress = (PVOID)0x180000040ull;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    ep.ContextRecord = &ctx;

    std::string j = Build(&ep, &s);

    EXPECT_NE(j.find("\"schema\": 12"), std::string::npos);
    auto modxPos = j.find("{ \"name\": \"ModX.dll\", \"hits\": 2, \"kind\": \"mod\" }");
    EXPECT_NE(modxPos, std::string::npos);
    EXPECT_NE(j.find("{ \"name\": \"Cyberpunk2077.exe\", \"hits\": 1, \"kind\": \"game\" }"), std::string::npos);
    EXPECT_NE(j.find("{ \"name\": \"kernelbase.dll\", \"hits\": 1, \"kind\": \"system\" }"), std::string::npos);
    EXPECT_LT(modxPos, j.find("\"name\": \"Cyberpunk2077.exe\", \"hits\": 1"));
    EXPECT_LT(modxPos, j.find("\"name\": \"kernelbase.dll\""));
    EXPECT_NE(j.find("\"stackModulesScanned\": 512"), std::string::npos);
    EXPECT_NE(j.find("\"stackModulesOverflow\": false"), std::string::npos);
}

TEST(JsonSidecar, StackModulesEmptyWithoutContext) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    AddModule(s, 0x140000000ull, 0x10000000, "Cyberpunk2077.exe", ModuleKind::Game);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    std::string j = Build(&ep, &s);
    EXPECT_NE(j.find("\"stackModules\": []"), std::string::npos);
    EXPECT_NE(j.find("\"stackModulesScanned\": 0"), std::string::npos);
    EXPECT_NE(j.find("\"callStackMods\": []"), std::string::npos);
    EXPECT_NE(j.find("\"objectsInFlight\": []"), std::string::npos);
    EXPECT_NE(j.find("\"archiveConflicts\": { \"curated\": { \"active\": [], "
                     "\"missingDeps\": [], \"dbModCount\": 0, \"truncated\": 0 } }"),
              std::string::npos);
    EXPECT_NE(j.find("\"setupIntegrity\": { \"issues\": [], \"truncated\": 0 }"),
              std::string::npos);
}

TEST(JsonSidecar, NullInputsProduceValidShell) {
    CrashFingerprint fp{};
    std::strncpy(fp.primaryId, "00000000", sizeof(fp.primaryId) - 1);
    SetLastFingerprint(fp);
    std::string j = Build(nullptr, nullptr);
    EXPECT_NE(j.find("\"crashId\": \"00000000\""), std::string::npos);
    EXPECT_NE(j.find("\"installedMods\": []"), std::string::npos);
    EXPECT_NE(j.find("\"faultingModule\": null"), std::string::npos);
}

TEST(JsonSidecar, ModsChangedSinceLastLaunchEmits) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    s.modDiff.priorFilePresent = true;
    s.modDiff.parsedOk = true;
    redscope::snap::ModSnapshot added;
    added.name = "Set Bonuses";
    added.version = "2.3.3.0";
    s.modDiff.added.push_back(added);
    redscope::snap::ModSnapshot up0, up1;
    up0.name = "TweakXL"; up0.version = "1.10";
    up1.name = "TweakXL"; up1.version = "1.11";
    s.modDiff.updated.push_back({up0, up1});
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    std::string j = Build(&ep, &s);
    EXPECT_NE(j.find("\"modsChangedSinceLastLaunch\""), std::string::npos);
    EXPECT_NE(j.find("\"Set Bonuses\""), std::string::npos);
    EXPECT_NE(j.find("\"2.3.3.0\""), std::string::npos);
    EXPECT_NE(j.find("\"from\": \"1.10\", \"to\": \"1.11\""), std::string::npos);
}

TEST(JsonSidecar, ObjectsInFlightEmitsRegClassModFields) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;

    redscope::rtti::InFlightSet inFlight;
    inFlight.count = 2;
    std::strncpy(inFlight.items[0].reg, "RCX", sizeof(inFlight.items[0].reg) - 1);
    std::strncpy(inFlight.items[0].className, "gamedataItem_Record",
                 sizeof(inFlight.items[0].className) - 1);
    inFlight.items[0].modFields = 2;
    std::strncpy(inFlight.items[1].reg, "RBX", sizeof(inFlight.items[1].reg) - 1);
    std::strncpy(inFlight.items[1].className, "PlayerPuppet",
                 sizeof(inFlight.items[1].className) - 1);
    inFlight.items[1].modFields = 0;

    redscope::PreallocatedBuffer buf;
    buf.Reserve(1 << 16);
    BuildJsonSidecar(buf, &ep, &s, std::chrono::system_clock::now(), &inFlight);
    std::string j(buf.Data(), buf.Size());

    EXPECT_NE(j.find("\"objectsInFlight\": [{ \"reg\": \"RCX\", \"className\": "
                     "\"gamedataItem_Record\", \"modFields\": 2 }, { \"reg\": \"RBX\", "
                     "\"className\": \"PlayerPuppet\", \"modFields\": 0 }]"),
              std::string::npos);
    EXPECT_NE(j.find("\"schema\": 12"), std::string::npos);
}

TEST(JsonSidecar, ModsChangedNullWhenNoPriorFile) {
    CrashFingerprint fp{};
    SetLastFingerprint(fp);
    Snapshot s{};
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000005;
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    std::string j = Build(&ep, &s);
    EXPECT_NE(j.find("\"modsChangedSinceLastLaunch\": null"), std::string::npos);
}
