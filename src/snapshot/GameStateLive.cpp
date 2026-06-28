#include "GameStateLive.h"
#include "../util/FixedStr.h"
#include "../util/Time.h"
#include <cstring>
#include <mutex>

namespace redscope::snap {

namespace {

std::mutex     g_mutex;
GameStateLive  g_live;

}

void SetLiveState(const char* key, const char* value) noexcept {
    if (!key || key[0] == '\0') return;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (uint32_t i = 0; i < g_live.count; ++i) {
        if (std::strncmp(g_live.entries[i].key, key, kGameStateKeyCap) == 0) {
            CopyFixed(g_live.entries[i].value, kGameStateValCap, value);
            g_live.entries[i].setNs = NowNs();
            g_live.hasData = true;
            return;
        }
    }
    if (g_live.count >= kGameStateMaxEntries) return;
    GameStateEntry& e = g_live.entries[g_live.count++];
    CopyFixed(e.key,   kGameStateKeyCap, key);
    CopyFixed(e.value, kGameStateValCap, value);
    e.setNs = NowNs();
    g_live.hasData = true;
}

void CaptureLiveState(GameStateLive& out) noexcept {
    std::lock_guard<std::mutex> lk(g_mutex);
    out = g_live;
    out.snapshotNs = NowNs();
}

void ResetLiveStateForTest() noexcept {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_live = GameStateLive{};
}

}
