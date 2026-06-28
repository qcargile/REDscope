#pragma once
#include <cstdint>
#include <cstring>
#include <atomic>
#include "EngineStateLive.h"
#include "GameStateLive.h"
#include "GpuState.h"
#include "HardwareSpecs.h"
#include "CuratedConflicts.h"
#include "InstalledMods.h"
#include "PluginMetadata.h"
#include "ResourceLoaderSnapshot.h"
#include "RttiSnapshot.h"
#include "SessionHistory.h"
#include "SetupIntegrity.h"
#include "WrapChain.h"

namespace redscope {

struct ProcessMemory {
    uint64_t workingSetBytes = 0;
    uint64_t commitBytes     = 0;
    uint64_t virtualBytes    = 0;
    uint32_t handleCount     = 0;
};

constexpr size_t kMaxModules       = 256;
constexpr size_t kModuleNameCap    = 48;
constexpr size_t kModuleVersionCap = 24;

enum class ModuleKind : uint8_t {
    Unknown = 0,
    Mod     = 1,
    Game    = 2,
    System  = 3,
};

struct ModuleInfoFixed {
    uintptr_t  base         = 0;
    uint32_t   size         = 0;
    uint16_t   nameIndex    = 0;
    uint16_t   versionIndex = 0;
    ModuleKind kind         = ModuleKind::Unknown;
    uint8_t    _pad[3]      = {};
};

struct Snapshot {
    int64_t         captureTimestampNs = 0;
    int64_t         gameUptimeNs       = 0;
    char            red4extVersion[32]{};
    char            cp2077BuildString[64]{};
    ProcessMemory   memory;

    ModuleInfoFixed modules[kMaxModules]{};
    char            moduleNames[kMaxModules][kModuleNameCap]{};
    char            moduleVersions[kMaxModules][kModuleVersionCap]{};
    uint32_t        moduleCount    = 0;
    uint32_t        moduleOverflow = 0;

    snap::ModInventory inventory;

    snap::HardwareSpecs hardware;

    snap::PluginMetadata pluginMeta;

    snap::RttiSnapshot rttiSnapshot;

    snap::GpuState gpu;

    snap::WrapChainTable wrapChains;

    snap::ModDiff modDiff;

    snap::ArchiveConflicts archiveConflicts;

    snap::SetupIntegrity setupIntegrity;

    snap::EngineStateLive engineLive;
    snap::GameStateLive   gameStateLive;

    snap::ResourceLoaderSnapshot resourceLoader;
};

inline const ModuleInfoFixed* FindModuleByPC(const Snapshot& s, uintptr_t pc) noexcept {
    uint32_t lo = 0;
    uint32_t hi = s.moduleCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const ModuleInfoFixed& m = s.modules[mid];
        if (pc < m.base) {
            hi = mid;
        } else if (pc >= m.base + m.size) {
            lo = mid + 1;
        } else {
            return &m;
        }
    }
    return nullptr;
}

}
