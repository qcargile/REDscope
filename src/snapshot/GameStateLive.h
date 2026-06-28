#pragma once
#include <cstdint>

namespace redscope::snap {

constexpr size_t kGameStateMaxEntries = 48;
constexpr size_t kGameStateKeyCap     = 32;
constexpr size_t kGameStateValCap     = 96;

struct GameStateEntry {
    char    key[kGameStateKeyCap]   = {};
    char    value[kGameStateValCap] = {};
    int64_t setNs                   = 0;
};

struct GameStateLive {
    GameStateEntry entries[kGameStateMaxEntries] = {};
    uint32_t       count                         = 0;
    int64_t        snapshotNs                    = 0;
    bool           hasData                       = false;
};

void SetLiveState(const char* key, const char* value) noexcept;
void CaptureLiveState(GameStateLive& out) noexcept;
void ResetLiveStateForTest() noexcept;

}
