#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace redscope::snap {

struct ModSnapshot {
    std::string name;
    std::string version;
};

struct ModDiff {
    std::vector<ModSnapshot>                               added;
    std::vector<std::pair<ModSnapshot, ModSnapshot>>       updated;
    std::vector<ModSnapshot>                               removed;

    int64_t priorTimestampUnix = 0;
    bool    priorFilePresent   = false;
    bool    parsedOk           = false;
    uint32_t priorModCount     = 0;
};

struct PriorSession {
    bool                     present       = false;
    bool                     parsedOk      = false;
    int64_t                  timestampUnix = 0;
    std::vector<ModSnapshot> mods;
};

PriorSession ReadSessionPrevious(const std::filesystem::path& path);

bool WriteSessionPrevious(const std::filesystem::path& path,
                          int64_t timestampUnix,
                          const std::vector<ModSnapshot>& current);

ModDiff ComputeModDiff(const std::vector<ModSnapshot>& prior,
                       const std::vector<ModSnapshot>& current);

}
