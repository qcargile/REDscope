#include "HangWatchdog.h"

#include "../snapshot/Snapshot.h"
#include "../snapshot/EngineStateLive.h"
#include "../hooks/ScriptStack.h"
#include "../util/PreallocatedBuffer.h"
#include "../util/Time.h"
#include "../Logger.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

namespace redscope::handler {

namespace {

constexpr int32_t kEngineStateRunning = 3;

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime  = 1099511628211ULL;

uint64_t FnvStr(uint64_t h, const char* s) noexcept {
    if (!s) return h;
    while (*s) { h ^= static_cast<unsigned char>(*s++); h *= kFnvPrime; }
    return h;
}

uint64_t Mix64(uint64_t x) noexcept {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

void EncodeId(uint64_t h, char* out, size_t cap) noexcept {
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    if (cap < 9) { if (cap) out[0] = '\0'; return; }
    uint64_t v = Mix64(h);
    for (int i = 0; i < 8; ++i) {
        out[i] = alphabet[(v >> (59 - i * 5)) & 0x1F];
    }
    out[8] = '\0';
}

void AppendEscaped(std::string& out, const char* s) {
    if (!s) return;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

void AppendJsonStr(std::string& out, const char* s) {
    out += '"';
    AppendEscaped(out, s);
    out += '"';
}

struct StuckThread {
    uint32_t    tid       = 0;
    const char* name      = nullptr;
    int64_t     lastNs    = 0;
};

std::vector<StuckThread> CollectStuckThreads() {
    std::vector<StuckThread> out;
    const redscope::scriptstack::HeartbeatSlot* slots = nullptr;
    size_t count = 0;
    redscope::scriptstack::GetHeartbeatSlots(&slots, &count);
    if (!slots) return out;
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].threadId == 0) continue;
        out.push_back({ slots[i].threadId, slots[i].lastFullName, slots[i].lastEnteredNs });
    }
    std::sort(out.begin(), out.end(),
              [](const StuckThread& a, const StuckThread& b) { return a.lastNs > b.lastNs; });
    return out;
}

void ComputeHangId(const std::vector<StuckThread>& stuck, const char* buildId,
                   char* outId, size_t cap) {
    uint64_t h = kFnvOffset;
    std::vector<const char*> names;
    for (const auto& s : stuck) {
        if (s.name && s.name[0]) names.push_back(s.name);
    }
    std::sort(names.begin(), names.end(),
              [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    for (const char* n : names) { h = FnvStr(h, n); h ^= 0x1F; }
    if (names.empty()) h = FnvStr(h, "no-scripted-activity");
    h = FnvStr(h, buildId ? buildId : "");
    EncodeId(h, outId, cap);
}

std::wstring BuildHangPath(const std::filesystem::path& dir) {
    using clock = std::chrono::system_clock;
    auto now = clock::to_time_t(clock::now());
    std::tm tmv{}; localtime_s(&tmv, &now);
    wchar_t name[64];
    std::swprintf(name, 64, L"REDscope-%04d-%02d-%02d_%02d-%02d-%02d.crash",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return (dir / name).wstring();
}

void AppendEngineStateJson(std::string& j, const redscope::Snapshot* snap) {
    j += "  \"engineState\": ";
    if (snap && snap->engineLive.hasData) {
        const auto& e = snap->engineLive;
        j += "{ ";
        if (e.engineReadOk) {
            j += "\"state\": ";
            AppendJsonStr(j, redscope::snap::EngineStateName(e.engineStateRaw));
            j += ", \"scriptsLoaded\": "; j += (e.scriptsLoaded ? "true" : "false");
            j += ", \"closing\": ";       j += (e.isClosing ? "true" : "false");
            j += ", \"isEP1\": ";         j += (e.isEP1 ? "true" : "false");
        } else {
            j += "\"engineRead\": false";
        }
        if (e.gpuReadOk) {
            char gbuf[96];
            std::snprintf(gbuf, sizeof(gbuf),
                          ", \"gpuUsedMB\": %u, \"gpuBudgetMB\": %u, \"gpuTotalMB\": %u",
                          e.gpuUsedMB, e.gpuBudgetMB, e.gpuDedicatedMB);
            j += gbuf;
        }
        j += ", \"oomSuspected\": "; j += (e.oomSuspected ? "true" : "false");
        j += " },\n";
    } else {
        j += "null,\n";
    }
}

void WriteHangReport(const std::filesystem::path& crashesDir,
                     const redscope::Snapshot* snap,
                     const std::vector<StuckThread>& stuck,
                     int64_t silenceNs) {
    const char* buildId = (snap && snap->cp2077BuildString[0]) ? snap->cp2077BuildString : "";
    const char* r4Ver   = (snap && snap->red4extVersion[0]) ? snap->red4extVersion : "";

    char hangId[12] = {};
    ComputeHangId(stuck, buildId, hangId, sizeof(hangId));

    long long unixTime =
        static_cast<long long>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    uint32_t silenceSec = static_cast<uint32_t>(silenceNs / 1'000'000'000LL);

    std::wstring crashPath = BuildHangPath(crashesDir);
    std::wstring jsonPath  = crashPath + L".json";

    std::string j;
    j.reserve(2048);
    j += "{\n";
    j += "  \"schema\": 2,\n";
    j += "  \"tool\": \"REDscope\",\n";
    j += "  \"source\": \"hang-watchdog\",\n";
    char nbuf[64];
    std::snprintf(nbuf, sizeof(nbuf), "  \"crashTimeUnix\": %lld,\n", unixTime);
    j += nbuf;
    std::snprintf(nbuf, sizeof(nbuf), "  \"hangSilenceSeconds\": %u,\n", silenceSec);
    j += nbuf;
    j += "  \"crashId\": "; AppendJsonStr(j, hangId); j += ",\n";
    j += "  \"looseId\": "; AppendJsonStr(j, hangId); j += ",\n";
    j += "  \"fingerprint\": { \"primary\": \"";
    j += hangId;
    j += "\", \"loose\": \"";
    j += hangId;
    j += "\", \"modSet\": \"0000000000000000\", \"faultSiteUnreliable\": false, \"hasModule\": false },\n";
    j += "  \"exception\": null,\n";
    j += "  \"faultingModule\": null,\n";
    j += "  \"stackModules\": [],\n";
    j += "  \"stackModulesScanned\": 0,\n";
    j += "  \"stackModulesOverflow\": false,\n";
    j += "  \"buildId\": "; AppendJsonStr(j, buildId); j += ",\n";
    j += "  \"red4extVersion\": "; AppendJsonStr(j, r4Ver); j += ",\n";
    j += "  \"crashClass\": \"Hang\",\n";
    AppendEngineStateJson(j, snap);
    j += "  \"scriptedFrames\": [";
    {
        bool first = true;
        for (const auto& s : stuck) {
            if (!s.name || !s.name[0]) continue;
            if (!first) j += ", ";
            first = false;
            AppendJsonStr(j, s.name);
        }
    }
    j += "],\n";
    j += "  \"installedMods\": []\n";
    j += "}\n";

    redscope::WriteBytesToFile(jsonPath.c_str(), j.data(), j.size());

    std::string t;
    t.reserve(1024);
    t += "================================================================================\n";
    t += " REDscope Hang Report\n";
    t += "================================================================================\n";
    t += "LIKELY CULPRIT: hang/freeze - the game stopped responding (no scripted activity)\n";
    char idline[96];
    std::snprintf(idline, sizeof(idline), "CRASH ID: %s (loose %s)\n", hangId, hangId);
    t += idline;
    t += "  fingerprint ";
    t += hangId;
    t += " / ";
    t += hangId;
    t += "  modset 0000000000000000\n\n";
    t += "--- Crash ---------------------------------------------------------------------\n";
    t += "Exception : none (hang/freeze - not an exception)\n";
    char silline[96];
    std::snprintf(silline, sizeof(silline), "Hang      : no scripted activity for %u s\n", silenceSec);
    t += silline;
    t += "Class     : Hang\n";
    t += "Cyberpunk : ";
    t += buildId;
    t += "\n\n";
    t += "--- LAST ACTIVE ---------------------------------------------------------------\n";
    t += "Scripted code (per thread, most-recent first):\n";
    if (stuck.empty()) {
        t += "  (no scripted activity recorded this session)\n";
    } else {
        int64_t nowNs = NowNs();
        for (const auto& s : stuck) {
            double agoS = (double)(nowNs - s.lastNs) / 1e9;
            char row[256];
            std::snprintf(row, sizeof(row), "  tid=%-6u %-56s (-%07.3fs ago)\n",
                          s.tid, (s.name && s.name[0]) ? s.name : "(none)", agoS);
            t += row;
        }
    }
    t += "\n";
    t += "--- Engine state (at detection) -----------------------------------------------\n";
    if (snap && snap->engineLive.hasData && snap->engineLive.engineReadOk) {
        t += "Engine state    : ";
        t += redscope::snap::EngineStateName(snap->engineLive.engineStateRaw);
        t += "\n";
        t += "Scripts loaded  : ";
        t += (snap->engineLive.scriptsLoaded ? "true" : "false");
        t += "\n";
    } else {
        t += "(engine state unavailable)\n";
    }
    t += "\n";
    t += "Detected by REDscope's non-attaching hang watchdog. The native stack of the\n";
    t += "frozen thread is not captured in this build; the scripted functions above are\n";
    t += "the lead. A freeze is not an exception, so no fault address exists.\n";

    redscope::WriteBytesToFile(crashPath.c_str(), t.data(), t.size());

    char log[160];
    std::snprintf(log, sizeof(log), "hang watchdog: detected (silence %u s, id %s), wrote report",
                  silenceSec, hangId);
    redscope::log::Warn(log);
    redscope::log::Flush();
}

bool g_reported = false;

}

HangVerdict EvaluateHang(int64_t maxLastEnteredNs, int64_t nowNs,
                         int32_t engineStateRaw, int64_t thresholdNs,
                         bool alreadyReported) noexcept {
    if (engineStateRaw != kEngineStateRunning) return HangVerdict::None;
    if (maxLastEnteredNs <= 0) return HangVerdict::None;
    int64_t silence = nowNs - maxLastEnteredNs;
    bool hung = silence > thresholdNs;
    if (hung && !alreadyReported) return HangVerdict::Detected;
    if (!hung && alreadyReported) return HangVerdict::Rearmed;
    return HangVerdict::None;
}

void CheckForHang(const redscope::Snapshot* snap,
                  const std::filesystem::path& crashesDir,
                  uint32_t thresholdSeconds,
                  bool enabled,
                  bool respectDebugger) noexcept {
    if (!enabled || thresholdSeconds == 0 || !snap) return;
    if (respectDebugger && ::IsDebuggerPresent()) return;

    auto stuck = CollectStuckThreads();
    int64_t maxLast = 0;
    for (const auto& s : stuck) {
        if (s.lastNs > maxLast) maxLast = s.lastNs;
    }

    int32_t engineState = (snap->engineLive.hasData && snap->engineLive.engineReadOk)
                              ? snap->engineLive.engineStateRaw : -1;
    int64_t thresholdNs = static_cast<int64_t>(thresholdSeconds) * 1'000'000'000LL;
    int64_t now = NowNs();

    HangVerdict v = EvaluateHang(maxLast, now, engineState, thresholdNs, g_reported);
    if (v == HangVerdict::Detected) {
        g_reported = true;
        WriteHangReport(crashesDir, snap, stuck, now - maxLast);
    } else if (v == HangVerdict::Rearmed) {
        g_reported = false;
    }
}

}
