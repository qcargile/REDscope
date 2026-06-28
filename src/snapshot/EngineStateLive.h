#pragma once
#include <cstdint>

namespace redscope::snap {

struct EngineStateLive {
    bool     hasData = false;

    bool     engineReadOk   = false;
    bool     isClosing      = false;
    bool     scriptsLoaded  = false;
    int32_t  engineStateRaw = -1;
    bool     isEP1          = false;

    bool     gpuReadOk      = false;
    uint32_t gpuUsedMB      = 0;
    uint32_t gpuBudgetMB    = 0;
    uint32_t gpuDedicatedMB = 0;

    bool     oomSuspected = false;
    char     oomBasis[64]{};
};

void CaptureEngineStateLive(EngineStateLive& out, uint64_t commitBytes) noexcept;

inline const char* EngineStateName(int32_t raw) noexcept {
    switch (raw) {
        case 0: return "Idle";
        case 1: return "BaseInitialization";
        case 2: return "Initialization";
        case 3: return "Running";
        default: return "(unknown)";
    }
}

}
