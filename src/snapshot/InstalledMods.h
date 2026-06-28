#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace redscope::snap {

enum class ModManager : uint8_t {
    None   = 0,
    MO2    = 1,
    Vortex = 2,
};

enum class ModType : uint8_t {
    Red4ext   = 0,
    Cet       = 1,
    RedScript = 2,
    TweakXL   = 3,
    Archive   = 4,
    Asi       = 5,
    Unknown   = 6,
};

struct InstalledMod {
    std::string              name;
    std::string              version;
    std::string              subpath;
    std::vector<std::string> archives;
    ModType                  type     = ModType::Unknown;
    bool                     enabled  = true;
};

constexpr size_t kMaxArchivesPerMod = 64;

constexpr size_t kMaxInstalledMods = 2048;

struct ModInventory {
    ModManager                manager = ModManager::None;
    std::string               managerDetails;
    std::vector<InstalledMod> mods;
    uint32_t                  truncated = 0;
};

ModInventory EnumerateInstalled(
    const std::filesystem::path& gameRoot,
    const std::optional<std::filesystem::path>& mo2RootOverride = std::nullopt);

std::optional<std::unordered_set<std::string>> CollectMO2OverlayRelpaths(
    const std::optional<std::filesystem::path>& mo2RootOverride = std::nullopt);

}
