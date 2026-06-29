#include "SnapshotWorker.h"
#include "EngineStateLive.h"
#include "GameStateLive.h"
#include "GpuState.h"
#include "HardwareSpecs.h"
#include "SessionHistory.h"
#include "WrapChain.h"
#include "../Plugin.h"
#include "InstalledMods.h"
#include "PluginMetadata.h"
#include "RttiSnapshot.h"
#include "../Logger.h"
#include "../handler/HangWatchdog.h"
#include "../hooks/Hooks.h"
#include "../hooks/DispatchHook.h"
#include "../modules/ModuleEnumerator.h"
#include "../util/FixedStr.h"
#include "../util/Time.h"
#include "CleanCore.h"
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <string>
#include <filesystem>
#include <optional>
#include <unordered_set>

namespace redscope::snap {
namespace {

Snapshot               g_buffers[2];
std::atomic<Snapshot*> g_published{nullptr};
int                    g_writeIndex = 0;
std::atomic<bool>      g_run{false};
std::thread            g_thread;
int64_t                g_initNs = 0;
uint64_t               g_cacheGen = 1;
uint64_t               g_bufferCacheGen[2] = { 0, 0 };

void FillProcessMemory(ProcessMemory& m, MEMORYSTATUSEX& memOut) {
    HANDLE proc = ::GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(proc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.workingSetBytes = pmc.WorkingSetSize;
        m.commitBytes     = pmc.PrivateUsage;
    }
    memOut.dwLength = sizeof(memOut);
    if (::GlobalMemoryStatusEx(&memOut)) {
        m.virtualBytes = memOut.ullTotalVirtual - memOut.ullAvailVirtual;
    }
    DWORD handles = 0;
    if (::GetProcessHandleCount(proc, &handles)) {
        m.handleCount = handles;
    }
}

void FillStaticVersions(Snapshot& s) {
    if (s.red4extVersion[0] == '\0') {
        HMODULE r4 = ::GetModuleHandleW(L"RED4ext.dll");
        if (r4) {
            wchar_t path[MAX_PATH] = {};
            ::GetModuleFileNameW(r4, path, MAX_PATH);
            CopyFixed(s.red4extVersion, sizeof(s.red4extVersion),
                     redscope::QueryFileVersionUtf8(path).c_str());
        }
    }
    if (s.cp2077BuildString[0] == '\0') {
        HMODULE exe = ::GetModuleHandleW(L"Cyberpunk2077.exe");
        if (exe) {
            wchar_t path[MAX_PATH] = {};
            ::GetModuleFileNameW(exe, path, MAX_PATH);
            CopyFixed(s.cp2077BuildString, sizeof(s.cp2077BuildString),
                     redscope::QueryFileVersionUtf8(path).c_str());
            if (s.cp2077BuildString[0] == '\0') {
                MODULEINFO mi{};
                if (::GetModuleInformation(::GetCurrentProcess(), exe, &mi, sizeof(mi))) {
                    std::snprintf(s.cp2077BuildString, sizeof(s.cp2077BuildString),
                                  "build-img-%08X", (unsigned)mi.SizeOfImage);
                }
            }
        }
    }
}

struct PathRoots {
    wchar_t gameRoot[MAX_PATH] = {};
    size_t  gameRootLen = 0;
    wchar_t windowsRoot[MAX_PATH] = {};
    size_t  windowsRootLen = 0;
    bool    initialized = false;
};
PathRoots g_roots;

void InitPathRoots(HANDLE proc) {
    if (g_roots.initialized) return;
    (void)proc;

    HMODULE exe = ::GetModuleHandleW(L"Cyberpunk2077.exe");
    if (exe) {
        wchar_t exePath[MAX_PATH] = {};
        if (::GetModuleFileNameW(exe, exePath, MAX_PATH) > 0) {
            for (int i = 0; i < 3; ++i) {
                wchar_t* slash = ::wcsrchr(exePath, L'\\');
                if (!slash) break;
                *slash = L'\0';
            }
            ::wcscpy_s(g_roots.gameRoot, MAX_PATH, exePath);
            g_roots.gameRootLen = ::wcslen(g_roots.gameRoot);
        }
    }

    UINT n = ::GetSystemWindowsDirectoryW(g_roots.windowsRoot, MAX_PATH);
    if (n > 0 && n < MAX_PATH) g_roots.windowsRootLen = n;

    g_roots.initialized = true;
}

bool StartsWithDir(const wchar_t* path, const wchar_t* root, size_t rootLen) {
    if (rootLen == 0) return false;
    if (::_wcsnicmp(path, root, rootLen) != 0) return false;
    wchar_t boundary = path[rootLen];
    return boundary == L'\\' || boundary == L'/' || boundary == L'\0';
}

ModuleKind ClassifyPath(const wchar_t* path) {
    if (!path || !path[0]) return ModuleKind::Unknown;

    if (StartsWithDir(path, g_roots.windowsRoot, g_roots.windowsRootLen)) {
        return ModuleKind::System;
    }

    if (g_roots.gameRootLen > 0 && StartsWithDir(path, g_roots.gameRoot, g_roots.gameRootLen)) {
        const wchar_t* rel = path + g_roots.gameRootLen + 1;
        if (::_wcsnicmp(rel, L"red4ext\\",         8)  == 0) return ModuleKind::Mod;
        if (::_wcsnicmp(rel, L"bin\\x64\\plugins\\", 16) == 0) return ModuleKind::Mod;
        if (::_wcsicmp(rel, L"bin\\x64\\winmm.dll") == 0) return ModuleKind::Mod;
        return ModuleKind::Game;
    }

    return ModuleKind::Unknown;
}

struct ModuleListCache {
    ModuleInfoFixed modules[kMaxModules]{};
    char            moduleNames[kMaxModules][kModuleNameCap]{};
    char            moduleVersions[kMaxModules][kModuleVersionCap]{};
    uint32_t        moduleCount    = 0;
    uint32_t        moduleOverflow = 0;
};

void FillModuleList(ModuleListCache& s) {
    HANDLE proc = ::GetCurrentProcess();
    InitPathRoots(proc);

    constexpr size_t kHandleBufferSize = 1024;
    HMODULE handles[kHandleBufferSize];
    DWORD needed = 0;
    if (!::EnumProcessModules(proc, handles, (DWORD)sizeof(handles), &needed)) {
        s.moduleCount    = 0;
        s.moduleOverflow = 0;
        return;
    }

    const size_t reported = needed / sizeof(HMODULE);
    const size_t got      = (std::min)(reported, kHandleBufferSize);
    const size_t take     = (std::min)(got, kMaxModules);

    size_t written = 0;
    for (size_t i = 0; i < take; ++i) {
        HMODULE h = handles[i];
        MODULEINFO mi{};
        if (!::GetModuleInformation(proc, h, &mi, sizeof(mi))) continue;

        ModuleInfoFixed& out = s.modules[written];
        out.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        out.size = mi.SizeOfImage;

        wchar_t baseNameW[kModuleNameCap + 16] = {};
        DWORD baseLen = ::GetModuleBaseNameW(proc, h, baseNameW,
                                             (DWORD)(sizeof(baseNameW) / sizeof(wchar_t)));
        if (baseLen == 0) baseNameW[0] = L'\0';

        char* nameDst = s.moduleNames[written];
        int nameBytes = ::WideCharToMultiByte(CP_UTF8, 0, baseNameW, -1,
                                              nameDst, (int)kModuleNameCap,
                                              nullptr, nullptr);
        if (nameBytes == 0) {
            nameDst[0] = '\0';
        } else {
            nameDst[kModuleNameCap - 1] = '\0';
        }

        wchar_t fullPath[MAX_PATH] = {};
        if (::GetModuleFileNameW(h, fullPath, MAX_PATH) > 0) {
            std::string ver = redscope::QueryFileVersionUtf8(fullPath);
            CopyFixed(s.moduleVersions[written], kModuleVersionCap, ver.c_str());
            out.kind = ClassifyPath(fullPath);
        } else {
            s.moduleVersions[written][0] = '\0';
            out.kind = ModuleKind::Unknown;
        }

        out.nameIndex    = (uint16_t)written;
        out.versionIndex = (uint16_t)written;
        ++written;
    }

    std::sort(&s.modules[0], &s.modules[written],
        [](const ModuleInfoFixed& a, const ModuleInfoFixed& b){ return a.base < b.base; });

    s.moduleCount    = (uint32_t)written;
    s.moduleOverflow = reported > kMaxModules ? (uint32_t)(reported - kMaxModules) : 0;
}

ModuleListCache g_cachedModuleList;
int64_t         g_moduleListFreshNs  = 0;
size_t          g_lastModuleRawCount = 0;
constexpr int64_t kModuleListRefreshNs = int64_t(30) * 1'000'000'000;

void RefreshModuleListIfStale() {
    HMODULE handles[1024];
    DWORD needed = 0;
    size_t rawCount = 0;
    if (::EnumProcessModules(::GetCurrentProcess(), handles, (DWORD)sizeof(handles), &needed)) {
        rawCount = needed / sizeof(HMODULE);
    }
    int64_t now = NowNs();
    const bool stale = (g_moduleListFreshNs == 0)
                    || (rawCount != g_lastModuleRawCount)
                    || ((now - g_moduleListFreshNs) >= kModuleListRefreshNs);
    if (!stale) return;
    FillModuleList(g_cachedModuleList);
    g_lastModuleRawCount = rawCount;
    g_moduleListFreshNs  = now;
}

constexpr int64_t kInventoryRefreshNs = int64_t(5) * 60 * 1'000'000'000;
ModInventory   g_cachedInventory;
int64_t        g_inventoryFreshNs = 0;

PluginMetadata g_cachedPluginMeta;
int64_t        g_pluginMetaFreshNs = 0;

HardwareSpecs g_cachedHardware;

GpuState g_cachedGpu;

WrapChainTable g_cachedWrapChains;
int64_t        g_wrapChainsFreshNs = 0;

ModDiff g_cachedModDiff;
bool    g_modDiffComputed = false;

ConflictDB        g_conflictDB;
ArchiveConflicts  g_cachedArchiveConflicts;
int64_t           g_archiveConflictsFreshNs = 0;
bool              g_lastIsEP1 = false;
bool              g_lastEngineReadOk = false;
bool              g_lastEvalIsEP1 = false;
std::atomic<bool> g_crashInProgress{false};

SetupIntegrity    g_cachedSetupIntegrity;
int64_t           g_setupIntegrityFreshNs = 0;
size_t            g_lastSetupModuleRawCount = 0;

void ComputeModDiffOnce() {
    if (g_modDiffComputed) return;
    if (g_cachedInventory.mods.empty()) return;
    const auto& pluginDir = redscope::PluginDir();
    if (pluginDir.empty()) return;

    auto historyPath = pluginDir / "crashes" / "session.previous";
    try {
        auto prior = ReadSessionPrevious(historyPath);

        std::vector<ModSnapshot> current;
        current.reserve(g_cachedInventory.mods.size());
        for (const auto& m : g_cachedInventory.mods) {
            current.push_back({ m.name, m.version });
        }

        g_cachedModDiff = ComputeModDiff(prior.mods, current);
        g_cachedModDiff.priorFilePresent = prior.present && prior.parsedOk;
        g_cachedModDiff.priorTimestampUnix = prior.timestampUnix;

        using namespace std::chrono;
        int64_t nowUnix = system_clock::to_time_t(system_clock::now());
        WriteSessionPrevious(historyPath, nowUnix, current);
    } catch (...) {
        return;
    }
    g_modDiffComputed = true;
}

LooseFileReport DetectLooseFiles() {
    static LooseFileReport s_cached;
    static bool s_computed = false;
    if (s_computed) return s_cached;

    LooseFileReport rep;
    if (g_roots.gameRootLen == 0) return rep;

    std::optional<std::unordered_set<std::string>> overlay = CollectMO2OverlayRelpaths();

    cleanslate::CleanReport cr =
        cleanslate::Scan(std::wstring(g_roots.gameRoot), overlay ? &*overlay : nullptr);
    rep.readOk = cr.ok;
    switch (cr.manager) {
        case cleanslate::Manager::MO2:    rep.manager = ModManager::MO2;    break;
        case cleanslate::Manager::Vortex: rep.manager = ModManager::Vortex; break;
        default:                          rep.manager = ModManager::None;   break;
    }
    rep.scannedCount = cr.scanned;
    rep.looseCount   = cr.loose + cr.orphan;
    for (const auto& f : cr.findings) {
        if (rep.offenders.size() >= kMaxLooseOffenders) break;
        rep.offenders.push_back(f.relPath);
    }

    if (rep.manager == ModManager::MO2) {
        if (overlay) {
            constexpr uint32_t kSanityFloor = 200;
            constexpr uint32_t kSanityPct   = 50;
            const bool canaryMissing =
                overlay->find("red4ext\\plugins\\redscope\\redscope.dll") == overlay->end();
            const bool implausible =
                rep.looseCount > kSanityFloor && rep.scannedCount > 0 &&
                (rep.looseCount * 100u / rep.scannedCount) > kSanityPct;
            rep.mo2Unreliable = canaryMissing || implausible;
        } else if (rep.looseCount > 0) {
            std::wstring root(g_roots.gameRoot);
            while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
            std::wstring canary = root + L"\\red4ext\\plugins\\REDscope\\REDscope.dll";
            rep.mo2Unreliable =
                cleanslate::ProbeReference(canary, root) != cleanslate::ReferenceVerdict::ResolvedOutsideRoot;
        }
    }

    s_cached = rep;
    s_computed = true;
    return rep;
}

void RefreshInventoryIfStale() {
    int64_t now = NowNs();
    if (g_inventoryFreshNs != 0 && (now - g_inventoryFreshNs) < kInventoryRefreshNs) return;
    if (g_roots.gameRootLen == 0) return;
    try {
        g_cachedInventory = EnumerateInstalled(std::filesystem::path(g_roots.gameRoot));
        g_inventoryFreshNs = now;
        ComputeModDiffOnce();
        ++g_cacheGen;
    } catch (...) {
    }
}

void RefreshWrapChainsIfStale() {
    int64_t now = NowNs();
    if (g_wrapChainsFreshNs != 0 && (now - g_wrapChainsFreshNs) < kInventoryRefreshNs) return;
    if (g_roots.gameRootLen == 0) return;
    try {
        g_cachedWrapChains = CaptureWrapChains(std::filesystem::path(g_roots.gameRoot),
                                                g_cachedInventory);
        g_wrapChainsFreshNs = now;
        ++g_cacheGen;
    } catch (...) {
    }
}

void RefreshPluginMetaIfStale() {
    int64_t now = NowNs();
    if (g_pluginMetaFreshNs != 0 && (now - g_pluginMetaFreshNs) < kInventoryRefreshNs) return;
    if (g_roots.gameRootLen == 0) return;
    try {
        g_cachedPluginMeta = EnumeratePluginMetadata(std::filesystem::path(g_roots.gameRoot));
        g_pluginMetaFreshNs = now;
        ++g_cacheGen;
    } catch (...) {
    }
}

void RefreshArchiveConflictsIfStale() {
    int64_t now = NowNs();
    const bool stale = (g_archiveConflictsFreshNs == 0)
                    || ((now - g_archiveConflictsFreshNs) >= kInventoryRefreshNs)
                    || (g_lastIsEP1 != g_lastEvalIsEP1);
    if (!stale) return;
    try {
        ConflictDB fresh;
        LoadConflictDir(fresh, redscope::PluginDir() / "conflicts");
        g_conflictDB = std::move(fresh);

        std::vector<std::string> installedArchives;
        for (const auto& m : g_cachedInventory.mods) {
            if (!m.enabled) continue;
            for (const auto& a : m.archives) installedArchives.push_back(a);
        }
        g_cachedArchiveConflicts =
            EvaluateConflicts(g_conflictDB, installedArchives, g_lastIsEP1, g_lastEngineReadOk);
        g_lastEvalIsEP1 = g_lastIsEP1;
        g_archiveConflictsFreshNs = now;
        ++g_cacheGen;
    } catch (...) {
    }
}

bool IsModuleLoaded(const ModuleListCache& ml, const char* basename) {
    const uint32_t count = ml.moduleCount < kMaxModules ? ml.moduleCount : kMaxModules;
    for (uint32_t i = 0; i < count; ++i) {
        const char* n = ml.moduleNames[i];
        size_t j = 0;
        for (;; ++j) {
            char a = n[j], b = basename[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            if (a == '\0') return true;
        }
    }
    return false;
}

void RefreshSetupIntegrityIfStale() {
    int64_t now = NowNs();
    const bool stale = (g_setupIntegrityFreshNs == 0)
                    || ((now - g_setupIntegrityFreshNs) >= kInventoryRefreshNs)
                    || (g_lastModuleRawCount != g_lastSetupModuleRawCount);
    if (!stale) return;
    try {
        FrameworkPresence fw;
        fw.cet = IsModuleLoaded(g_cachedModuleList, "cyber_engine_tweaks.asi")
              || !g_cachedPluginMeta.cetVersion.empty();
        fw.tweakXL = IsModuleLoaded(g_cachedModuleList, "tweakxl.dll")
                  || g_cachedPluginMeta.byDll.count("tweakxl.dll") > 0;
        LooseFileReport loose = DetectLooseFiles();
        g_cachedSetupIntegrity =
            ComputeSetupIntegrity(g_cachedInventory, fw, g_cachedPluginMeta.failedLoadCount, loose);
        g_lastSetupModuleRawCount = g_lastModuleRawCount;
        g_setupIntegrityFreshNs = now;
        ++g_cacheGen;
    } catch (...) {
    }
}


void Tick() {
    if (g_crashInProgress.load(std::memory_order_acquire)) return;
    Snapshot& target = g_buffers[g_writeIndex];
    target.captureTimestampNs = NowNs();
    target.gameUptimeNs       = target.captureTimestampNs - g_initNs;
    FillStaticVersions(target);
    MEMORYSTATUSEX memStatus{};
    FillProcessMemory(target.memory, memStatus);
    CaptureEngineStateLive(target.engineLive, target.memory.commitBytes);
    g_lastIsEP1 = target.engineLive.isEP1;
    g_lastEngineReadOk = target.engineLive.engineReadOk;
    {
        if (memStatus.ullTotalPageFile > 0) {
            uint64_t commitPct = (target.memory.commitBytes * 100ull) / memStatus.ullTotalPageFile;
            if (commitPct >= 90) {
                char val[48];
                std::snprintf(val, sizeof(val), "commit %llu%% (%llu/%llu MB)",
                              (unsigned long long)commitPct,
                              (unsigned long long)(target.memory.commitBytes >> 20),
                              (unsigned long long)(memStatus.ullTotalPageFile >> 20));
                SetLiveState("memory.pressure90", val);
            }
        }
        if (target.engineLive.gpuReadOk && target.engineLive.gpuBudgetMB > 0) {
            uint64_t vramPct = (static_cast<uint64_t>(target.engineLive.gpuUsedMB) * 100ull)
                               / target.engineLive.gpuBudgetMB;
            if (vramPct >= 90) {
                char val[48];
                std::snprintf(val, sizeof(val), "vram %llu%% (%u/%u MB)",
                              (unsigned long long)vramPct,
                              target.engineLive.gpuUsedMB,
                              target.engineLive.gpuBudgetMB);
                SetLiveState("vram.pressure90", val);
            }
        }
    }
    CaptureLiveState(target.gameStateLive);
    RefreshModuleListIfStale();
    std::memcpy(target.modules, g_cachedModuleList.modules, sizeof(target.modules));
    std::memcpy(target.moduleNames, g_cachedModuleList.moduleNames, sizeof(target.moduleNames));
    std::memcpy(target.moduleVersions, g_cachedModuleList.moduleVersions, sizeof(target.moduleVersions));
    target.moduleCount    = g_cachedModuleList.moduleCount;
    target.moduleOverflow = g_cachedModuleList.moduleOverflow;
    RefreshInventoryIfStale();
    RefreshPluginMetaIfStale();
    RefreshWrapChainsIfStale();
    RefreshArchiveConflictsIfStale();
    RefreshSetupIntegrityIfStale();
    if (g_bufferCacheGen[g_writeIndex] != g_cacheGen) {
        target.inventory        = g_cachedInventory;
        target.hardware         = g_cachedHardware;
        target.pluginMeta       = g_cachedPluginMeta;
        target.gpu              = g_cachedGpu;
        target.wrapChains       = g_cachedWrapChains;
        target.modDiff          = g_cachedModDiff;
        target.archiveConflicts = g_cachedArchiveConflicts;
        target.setupIntegrity   = g_cachedSetupIntegrity;
        g_bufferCacheGen[g_writeIndex] = g_cacheGen;
    }
    g_published.store(&target, std::memory_order_release);
    g_writeIndex = (g_writeIndex + 1) & 1;
    try { redscope::hooks::InstallAll(); } catch (...) {}

    try {
        redscope::hooks::dispatch::ResolveHeartbeatNames();
        const auto& cfg = redscope::GetConfig();
        redscope::handler::CheckForHang(&target, redscope::PluginDir() / "crashes",
                                        cfg.hangThresholdSeconds, cfg.hangWatchdogEnabled,
                                        cfg.respectDebugger);
    } catch (...) {}
}

void Run(uint32_t intervalMs) {
    while (g_run.load(std::memory_order_relaxed)) {
        ::Sleep(intervalMs);
        try { Tick(); } catch (...) {}
    }
}

}

void Start(uint32_t intervalMs) {
    if (intervalMs == 0) intervalMs = 500;
    if (g_thread.joinable()) {
        g_run.store(false);
        g_thread.join();
    }
    g_initNs = NowNs();
    try { g_cachedHardware = CaptureHardwareSpecs(); } catch (...) {}
    try { InitPathRoots(::GetCurrentProcess()); } catch (...) {}
    try { CaptureInto(g_cachedGpu, std::filesystem::path(g_roots.gameRoot)); } catch (...) {}
    try { Tick(); } catch (...) {}
    try { ::SymRefreshModuleList(::GetCurrentProcess()); } catch (...) {}
    g_run.store(true);
    g_thread = std::thread(Run, intervalMs);
}

void Stop() {
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();
}

void FreezeForCrash() noexcept {
    g_crashInProgress.store(true, std::memory_order_release);
}

const Snapshot* Current() noexcept {
    return g_published.load(std::memory_order_acquire);
}

int64_t UptimeNsAtCrash() noexcept {
    return g_initNs != 0 ? (NowNs() - g_initNs) : 0;
}

const wchar_t* GameRoot() noexcept {
    return g_roots.gameRoot[0] ? g_roots.gameRoot : L"";
}

}
