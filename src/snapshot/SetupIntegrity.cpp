#include "SetupIntegrity.h"

#include <string>

namespace redscope::snap {

SetupIntegrity ComputeSetupIntegrity(const ModInventory& inventory,
                                     const FrameworkPresence& frameworks,
                                     uint32_t pluginLoadFailures,
                                     const LooseFileReport& loose) {
    SetupIntegrity result;

    uint32_t cetMods = 0;
    uint32_t tweakXLMods = 0;
    for (const auto& m : inventory.mods) {
        if (!m.enabled) continue;
        if (m.type == ModType::Cet)     ++cetMods;
        if (m.type == ModType::TweakXL) ++tweakXLMods;
    }

    auto addIssue = [&](const char* kind, std::string detail) {
        if (result.issues.size() >= kMaxSetupIssues) { ++result.truncated; return; }
        result.issues.push_back({ kind, std::move(detail) });
    };

    if (cetMods > 0 && !frameworks.cet) {
        addIssue("missing-framework",
                 std::to_string(cetMods) + " CET mod(s) installed but Cyber Engine Tweaks is not loaded.");
    }
    if (tweakXLMods > 0 && !frameworks.tweakXL) {
        addIssue("missing-framework",
                 std::to_string(tweakXLMods) + " TweakXL mod(s) installed but TweakXL is not loaded.");
    }
    if (pluginLoadFailures > 0) {
        addIssue("plugin-load-failure",
                 std::to_string(pluginLoadFailures) + " RED4ext plugin(s) failed to load; see the red4ext log.");
    }

    if (loose.readOk && loose.manager == ModManager::MO2 && loose.mo2Unreliable) {
        addIssue("loose-files-unverified",
                 "Couldn't verify game-folder mod files on this MO2/USVFS setup.");
    } else if (loose.readOk && loose.looseCount > 0) {
        std::string sample;
        for (size_t i = 0; i < loose.offenders.size() && i < 5; ++i) {
            sample += (i == 0 ? ": " : ", ") + loose.offenders[i];
        }
        if (loose.looseCount > loose.offenders.size()) sample += ", ...";
        if (loose.manager == ModManager::MO2) {
            addIssue("loose-files-mo2",
                     std::to_string(loose.looseCount) +
                     " mod file(s) are in the game folder but not overlaid by any enabled mod in your MO2 modlist - leftover or manually-installed files" + sample + ".");
        } else if (loose.manager == ModManager::None) {
            addIssue("loose-files-native",
                     std::to_string(loose.looseCount) +
                     " manually-installed mod file(s) in the game folder (no mod manager detected)" + sample + ".");
        }
    }

    return result;
}

}
