#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "InstalledMods.h"

namespace redscope::snap {

struct SetupIssue {
    std::string kind;
    std::string detail;
};

struct SetupIntegrity {
    std::vector<SetupIssue> issues;
    uint32_t                truncated = 0;
};

struct FrameworkPresence {
    bool cet     = false;
    bool tweakXL = false;
};

constexpr size_t kMaxSetupIssues     = 32;
constexpr size_t kMaxLooseOffenders  = 16;

struct LooseFileReport {
    ModManager               manager      = ModManager::None;
    uint32_t                 looseCount   = 0;
    uint32_t                 scannedCount = 0;
    std::vector<std::string> offenders;
    bool                     readOk       = false;
    bool                     mo2Unreliable = false;
};

SetupIntegrity ComputeSetupIntegrity(const ModInventory& inventory,
                                     const FrameworkPresence& frameworks,
                                     uint32_t pluginLoadFailures,
                                     const LooseFileReport& loose = LooseFileReport{});

}
