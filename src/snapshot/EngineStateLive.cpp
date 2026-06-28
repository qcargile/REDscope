#include "EngineStateLive.h"
#include "../util/SehGuardedRead.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifdef REDSCOPE_USE_RTTI
#include <RED4ext/GameEngine.hpp>
#endif

namespace redscope::snap {

namespace {

IDXGIAdapter3* g_adapter = nullptr;
uint32_t       g_dedicatedMB = 0;
bool           g_adapterTried = false;

void AcquireAdapter() noexcept {
    if (g_adapterTried) return;
    g_adapterTried = true;

    IDXGIFactory* factory = nullptr;
    if (FAILED(::CreateDXGIFactory(IID_PPV_ARGS(&factory))) || !factory) return;

    for (UINT i = 0; ; ++i) {
        IDXGIAdapter* cand = nullptr;
        if (factory->EnumAdapters(i, &cand) != S_OK) break;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(cand->GetDesc(&desc)) && desc.VendorId != 0x1414) {
            IDXGIAdapter3* a3 = nullptr;
            if (SUCCEEDED(cand->QueryInterface(__uuidof(IDXGIAdapter3),
                                               reinterpret_cast<void**>(&a3)))) {
                g_adapter = a3;
                g_dedicatedMB = (uint32_t)(desc.DedicatedVideoMemory / (1024ull * 1024ull));
                cand->Release();
                break;
            }
        }
        cand->Release();
    }
    factory->Release();
}

void FillGpu(EngineStateLive& out) noexcept {
    AcquireAdapter();
    if (!g_adapter) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        const uint64_t mb = 1024ull * 1024ull;
        out.gpuUsedMB      = (uint32_t)(info.CurrentUsage / mb);
        out.gpuBudgetMB    = (uint32_t)(info.Budget / mb);
        out.gpuDedicatedMB = g_dedicatedMB;
        out.gpuReadOk = true;
    }
}

void FillOomHeuristic(EngineStateLive& out, uint64_t commitBytes) noexcept {
    const uint64_t mb = 1024ull * 1024ull;
    if (out.gpuReadOk && out.gpuBudgetMB > 0 &&
        (uint64_t)out.gpuUsedMB * 100 >= (uint64_t)out.gpuBudgetMB * 95) {
        out.oomSuspected = true;
        std::snprintf(out.oomBasis, sizeof(out.oomBasis),
                      "VRAM %u/%u MB", out.gpuUsedMB, out.gpuBudgetMB);
        return;
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (::GlobalMemoryStatusEx(&mem) && mem.ullTotalPageFile > 0 &&
        commitBytes * 100 >= mem.ullTotalPageFile * 95) {
        out.oomSuspected = true;
        std::snprintf(out.oomBasis, sizeof(out.oomBasis), "commit %llu/%llu MB",
                      (unsigned long long)(commitBytes / mb),
                      (unsigned long long)(mem.ullTotalPageFile / mb));
    }
}

#ifdef REDSCOPE_USE_RTTI
void FillEngine(EngineStateLive& out) noexcept {
    const RED4ext::CBaseEngine* base = RED4ext::CGameEngine::Get();
    if (!base) return;
    if (!redscope::LooksReadable(base, sizeof(RED4ext::CBaseEngine))) return;

    const auto* p = reinterpret_cast<const uint8_t*>(base);

    int32_t state = -1;
    if (!redscope::SehSafeReadValue(state, p + offsetof(RED4ext::CBaseEngine, engineState))) return;
    if (state < 0 || state > 3) return;
    out.engineStateRaw = state;

    int8_t terminating = 0;
    if (redscope::SehSafeReadValue(terminating, p + offsetof(RED4ext::CBaseEngine, terminating)))
        out.isClosing = (terminating != 0);

    uint8_t scriptsLoaded = 0;
    if (redscope::SehSafeReadValue(scriptsLoaded, p + offsetof(RED4ext::CBaseEngine, scriptsLoaded)))
        out.scriptsLoaded = (scriptsLoaded != 0);

    uint8_t ep1 = 0;
    if (redscope::SehSafeReadValue(ep1, p + offsetof(RED4ext::CBaseEngine, isEP1)))
        out.isEP1 = (ep1 != 0);

    out.engineReadOk = true;
}
#else
void FillEngine(EngineStateLive&) noexcept {}
#endif

}

void CaptureEngineStateLive(EngineStateLive& out, uint64_t commitBytes) noexcept {
    out = EngineStateLive{};
    FillEngine(out);
    FillGpu(out);
    FillOomHeuristic(out, commitBytes);
    out.hasData = out.engineReadOk || out.gpuReadOk;
}

}
