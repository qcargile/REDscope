#include "EngineDumpEnricher.h"
#include <simdjson.h>
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace redscope::snap {

namespace {

bool StartsWith(std::string_view s, std::string_view p) noexcept {
    return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' ||
                          s.back()  == '\r' || s.back()  == '\n')) s.remove_suffix(1);
    return s;
}

struct KV { std::string_view key; std::string_view value; };

KV ParseKV(std::string_view line) noexcept {
    line = Trim(line);
    if (line.size() < 5 || line.front() != '"') return {};

    size_t keyEnd = line.find('"', 1);
    if (keyEnd == std::string_view::npos) return {};

    if (keyEnd + 2 >= line.size() || line[keyEnd + 1] != ':') return {};

    std::string_view key = line.substr(1, keyEnd - 1);

    size_t at = key.find('@');
    if (at != std::string_view::npos) key = key.substr(0, at);

    std::string_view after = line.substr(keyEnd + 2);
    if (after.empty()) return {};

    std::string_view value;
    if (after.front() == '"') {
        size_t close = after.find('"', 1);
        if (close == std::string_view::npos) return {};
        value = after.substr(1, close - 1);
    } else {
        size_t stop = 0;
        while (stop < after.size() && after[stop] != ',' && after[stop] != '\r' &&
               after[stop] != '\n') ++stop;
        value = Trim(after.substr(0, stop));
    }
    return { key, value };
}

bool ParseBool(std::string_view v) noexcept {
    if (v.size() == 4 && (v[0] == 't' || v[0] == 'T')) return true;
    return false;
}

uint64_t ParseU64(std::string_view v) noexcept {
    if (v.empty()) return 0;
    uint64_t n = 0;
    auto res = std::from_chars(v.data(), v.data() + v.size(), n);
    if (res.ec != std::errc{}) return 0;
    return n;
}

uint32_t ParseU32(std::string_view v) noexcept {
    return (uint32_t)ParseU64(v);
}

double ParseDouble(std::string_view v) noexcept {
    if (v.empty()) return 0.0;
    double d = 0.0;
    auto res = std::from_chars(v.data(), v.data() + v.size(), d);
    if (res.ec != std::errc{}) return 0.0;
    return d;
}

bool ParseReportQueueStem(std::string_view stem,
                          uint32_t pid,
                          int& outYear, int& outMonth, int& outDay,
                          int& outHour, int& outMin, int& outSec,
                          uint32_t& outTid) {
    const std::string_view kPrefix = "Cyberpunk2077-";
    if (!StartsWith(stem, kPrefix)) return false;
    stem.remove_prefix(kPrefix.size());

    if (stem.size() < 17) return false;
    auto digits = [&](size_t off, size_t len, int& out) -> bool {
        out = 0;
        for (size_t i = 0; i < len; ++i) {
            char c = stem[off + i];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int Y, M, D, h, m, s;
    if (!digits(0, 4, Y)) return false;
    if (!digits(4, 2, M)) return false;
    if (!digits(6, 2, D)) return false;
    if (stem[8] != '-') return false;
    if (!digits(9,  2, h)) return false;
    if (!digits(11, 2, m)) return false;
    if (!digits(13, 2, s)) return false;
    if (stem[15] != '-') return false;

    stem.remove_prefix(16);
    size_t dash = stem.find('-');
    if (dash == std::string_view::npos) return false;
    uint64_t parsedPid = ParseU64(stem.substr(0, dash));
    if ((uint32_t)parsedPid != pid) return false;

    uint64_t parsedTid = ParseU64(stem.substr(dash + 1));
    outTid = (uint32_t)parsedTid;
    outYear = Y; outMonth = M; outDay = D;
    outHour = h; outMin   = m; outSec = s;
    return true;
}

std::chrono::system_clock::time_point DirTimestampToTimePoint(
    int Y, int M, int D, int h, int m, int s) {
    std::tm t{};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;
    t.tm_isdst = -1;
    time_t tt = std::mktime(&t);
    return std::chrono::system_clock::from_time_t(tt);
}

struct CrashHeaderInfo {
    bool ok = false;
    std::chrono::system_clock::time_point crashTime{};
    uint32_t pid = 0;
};

CrashHeaderInfo ParseCrashHeader(const std::filesystem::path& crashPath) {
    CrashHeaderInfo out;
    std::ifstream in(crashPath);
    if (!in) return out;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (StartsWith(line, "Time      :")) {
            int Y=0,M=0,D=0,h=0,m=0,s=0;
            if (std::sscanf(line.c_str() + 11, " %d-%d-%d %d:%d:%d",
                            &Y,&M,&D,&h,&m,&s) == 6) {
                out.crashTime = DirTimestampToTimePoint(Y,M,D,h,m,s);
            }
        } else if (StartsWith(line, "Engine dump:")) {
            auto pos = line.find("Cyberpunk2077-");
            if (pos != std::string::npos) {
                size_t tailStart = pos + std::strlen("Cyberpunk2077-");
                if (line.size() > tailStart + 16) {
                    const char* p = line.c_str() + tailStart + 16;
                    uint32_t val = 0;
                    while (*p >= '0' && *p <= '9') { val = val*10 + (*p - '0'); ++p; }
                    out.pid = val;
                }
            }
        } else if (line[0] == '-' || StartsWith(line, "===")) {
            if (out.crashTime.time_since_epoch().count() != 0 && out.pid != 0) break;
        }
    }
    out.ok = (out.crashTime.time_since_epoch().count() != 0) && (out.pid != 0);
    return out;
}

std::filesystem::path FindAttchText(const std::filesystem::path& reportDir) {
    std::filesystem::path attch = reportDir / "attch";
    std::error_code ec;
    if (!std::filesystem::exists(attch, ec)) return {};
    for (auto const& entry : std::filesystem::directory_iterator(attch, ec)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (StartsWith(name, "Cyberpunk2077.exe-") &&
            entry.path().extension() == ".txt") {
            return entry.path();
        }
    }
    return {};
}

std::filesystem::path FindSaveMetadataJson(const std::filesystem::path& reportDir) {
    std::filesystem::path attch = reportDir / "attch";
    std::error_code ec;
    if (!std::filesystem::exists(attch, ec)) return {};
    for (auto const& entry : std::filesystem::directory_iterator(attch, ec)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (StartsWith(name, "metadata.") && entry.path().extension() == ".json") {
            return entry.path();
        }
    }
    return {};
}

std::string SlurpFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    f.seekg(0, std::ios::beg);
    f.read(s.data(), static_cast<std::streamsize>(len));
    s.resize(static_cast<size_t>(f.gcount()));
    return s;
}

}

EngineDumpAttchFields ParseEngineDumpAttchText(std::string_view content) noexcept {
    EngineDumpAttchFields out;
    if (content.empty()) return out;

    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? content.substr(pos)
            : content.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? content.size() : (nl + 1);

        KV kv = ParseKV(line);
        if (kv.key.empty()) continue;

        auto key   = kv.key;
        auto value = kv.value;

        if (key == "uptimeSeconds")               out.uptimeSeconds = ParseU64(value);
        else if (key == "stopThreadID")           out.stopThreadId  = ParseU32(value);
        else if (key == "exceptionCode")          out.exceptionCode = (uint32_t)ParseU64(value);
        else if (key == "Engine/Scripts/CompileScriptsSuccess") {
            out.compileScriptsSuccess    = ParseBool(value);
            out.compileScriptsSuccessSet = true;
        }
        else if (key == "Game/LoadingStage")      out.loadingStage = std::string(value);
        else if (key == "Game/SessionDesc/IsLoadingSavedSession") {
            out.isLoadingSavedSession    = ParseBool(value);
            out.isLoadingSavedSessionSet = true;
        }
        else if (key == "Game/SessionDesc/WorldName") out.worldName = std::string(value);
        else if (key == "GlobalMode/IsGame")    { out.isGame = ParseBool(value);    out.isGameSet    = true; }
        else if (key == "GlobalMode/IsClosing") { out.isClosing = ParseBool(value); out.isClosingSet = true; }
        else if (key == "Engine/OOM")           { out.engineOOM = ParseBool(value); out.engineOOMSet = true; }
        else if (key == "Game/TransformAnimator/Components")        out.transformAnimatorComponents        = ParseU32(value);
        else if (key == "Game/TransformAnimator/RunningComponents") out.transformAnimatorRunningComponents = ParseU32(value);
        else if (key == "Game/Population/Registered")               out.populationRegistered               = ParseU32(value);
        else if (key == "Game/Population/Attached")                 out.populationAttached                 = ParseU32(value);
        else if (key == "Game/Interactions/HotSpots/CurrentCount")  out.hotSpotsCurrent                    = ParseU32(value);
        else if (key == "Game/Interactions/HotSpots/MaxCount")      out.hotSpotsMax                        = ParseU32(value);
        else if (key == "Game/Interactions/UniqueLayersCurrentCount") out.interactionsUniqueLayersCurrent  = ParseU32(value);
        else if (key == "Game/TransactionSystem/NumberOfItemsInPlayersInventory") out.playerInventoryItems = ParseU32(value);
        else if (key == "Game/TransactionSystem/NumberOfItemsInPlayersStash")     out.playerStashItems     = ParseU32(value);
        else if (key == "Game/StatsSystem/NumberOfStatsBundles")                  out.statsBundles         = ParseU32(value);
        else if (key == "Gpu/Device/UsedMemoryMB")   out.gpuUsedMemoryMB  = ParseU32(value);
        else if (key == "Gpu/Device/TotalMemoryMB")  out.gpuTotalMemoryMB = ParseU32(value);
        else if (key == "Streaming/MountState")              out.streamingMountState            = std::string(value);
        else if (key == "Streaming/LastObserverPosition")    out.streamingLastObserverPosition  = std::string(value);
        else if (key == "Engine/VersionWatermark")           out.engineVersionWatermark         = std::string(value);
    }

    out.hasData = (out.uptimeSeconds > 0) ||
                  !out.loadingStage.empty() ||
                  out.compileScriptsSuccessSet ||
                  !out.worldName.empty() ||
                  !out.engineVersionWatermark.empty();
    return out;
}

SaveMetadataFields ParseSaveMetadataJson(std::string_view json) noexcept {
    SaveMetadataFields out;
    if (json.empty()) return out;

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(json);
        auto doc = parser.iterate(padded);
        auto meta = doc["Data"]["metadata"];

        auto get_str = [&](const char* key) -> std::string {
            auto v = meta[key];
            if (v.error()) return "";
            std::string_view sv;
            if (v.get_string().get(sv)) return "";
            return std::string(sv);
        };
        auto get_double = [&](const char* key) -> double {
            auto v = meta[key];
            if (v.error()) return 0.0;
            double d = 0.0;
            if (v.get_double().get(d)) return 0.0;
            return d;
        };
        auto get_uint = [&](const char* key) -> uint64_t {
            auto v = meta[key];
            if (v.error()) return 0;
            uint64_t u = 0;
            if (v.get_uint64().get(u) == simdjson::SUCCESS) return u;
            double d = 0.0;
            if (v.get_double().get(d) == simdjson::SUCCESS) return (uint64_t)d;
            return 0;
        };
        auto get_bool = [&](const char* key, bool& outSet) -> bool {
            auto v = meta[key];
            if (v.error()) { outSet = false; return false; }
            bool b = false;
            if (v.get_bool().get(b) == simdjson::SUCCESS) { outSet = true; return b; }
            outSet = false;
            return false;
        };

        out.saveName       = get_str("name");
        out.buildID        = get_str("buildID");
        out.initialBuildID = get_str("initialBuildID");
        out.trackedQuest   = get_str("trackedQuest");
        out.locationName   = get_str("locationName");
        out.lifePath       = get_str("lifePath");
        out.difficulty     = get_str("difficulty");
        out.saveVersion    = (uint32_t)get_uint("saveVersion");
        out.gameVersion    = (uint32_t)get_uint("gameVersion");
        out.playerLevel    = (uint32_t)get_double("level");
        out.playTime       = get_double("playTime");
        out.isModded       = get_bool("isModded", out.isModdedSet);

        out.hasData = !out.buildID.empty() || !out.saveName.empty() ||
                      out.saveVersion > 0;
    } catch (...) {
    }
    return out;
}

std::filesystem::path FindReportQueueDir(
    const std::filesystem::path& reportQueueRoot,
    uint32_t pid,
    std::chrono::system_clock::time_point crashTime,
    std::chrono::seconds tolerance) {

    std::error_code ec;
    if (!std::filesystem::exists(reportQueueRoot, ec)) return {};

    std::filesystem::path bestMatch;
    std::chrono::seconds bestDelta{std::numeric_limits<int64_t>::max()};

    for (auto const& entry : std::filesystem::directory_iterator(reportQueueRoot, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto name = entry.path().filename().string();
        int Y=0,M=0,D=0,h=0,m=0,s=0; uint32_t tid=0;
        if (!ParseReportQueueStem(name, pid, Y, M, D, h, m, s, tid)) continue;
        auto ts = DirTimestampToTimePoint(Y, M, D, h, m, s);
        auto delta = std::chrono::abs(std::chrono::duration_cast<std::chrono::seconds>(
            ts - crashTime));
        if (delta <= tolerance && delta < bestDelta) {
            bestDelta   = delta;
            bestMatch   = entry.path();
        }
    }
    return bestMatch;
}

std::string RenderEngineStateSidecar(const EngineDumpAttchFields& attch,
                                     const SaveMetadataFields& meta) {
    std::ostringstream o;
    o << "================================================================================\n";
    o << " Engine state at crash (extracted from CDPR REDEngine ReportQueue)\n";
    o << "================================================================================\n\n";

    if (!attch.hasData && !meta.hasData) {
        o << "(no extractable signals found in the ReportQueue dump)\n";
        return o.str();
    }

    if (attch.hasData) {
        o << "--- Engine state ---\n";
        if (attch.uptimeSeconds > 0) {
            o << "uptimeSeconds         : " << attch.uptimeSeconds << "\n";
        }
        if (attch.compileScriptsSuccessSet) {
            o << "Scripts compiled OK   : " << (attch.compileScriptsSuccess ? "true" : "false")
              << "  (see Engine/Scripts/Loaded for actual load state)\n";
        }
        if (!attch.loadingStage.empty()) {
            o << "Loading stage         : " << attch.loadingStage << "\n";
        }
        if (attch.isLoadingSavedSessionSet) {
            o << "Loading saved session : " << (attch.isLoadingSavedSession ? "true" : "false") << "\n";
        }
        if (!attch.worldName.empty()) {
            o << "World                 : " << attch.worldName << "\n";
        }
        if (attch.isGameSet) {
            o << "Is in game            : " << (attch.isGame ? "true" : "false") << "\n";
        }
        if (attch.isClosingSet && attch.isClosing) {
            o << "Engine is closing     : true  (crash fired during shutdown)\n";
        }
        if (attch.engineOOMSet && attch.engineOOM) {
            o << "Engine OOM flag       : true  (<-- out-of-memory signaled by engine)\n";
        }
        o << "\n--- Scene density ---\n";
        if (attch.transformAnimatorComponents > 0) {
            o << "TransformAnimator     : " << attch.transformAnimatorComponents
              << " components (" << attch.transformAnimatorRunningComponents << " running)\n";
        }
        if (attch.populationRegistered > 0 || attch.populationAttached > 0) {
            o << "Population            : " << attch.populationRegistered << " registered, "
              << attch.populationAttached << " attached\n";
        }
        if (attch.hotSpotsCurrent > 0 || attch.hotSpotsMax > 0) {
            o << "Hotspots              : " << attch.hotSpotsCurrent
              << " current (max seen " << attch.hotSpotsMax << ")\n";
        }
        if (attch.interactionsUniqueLayersCurrent > 0) {
            o << "Interaction layers    : " << attch.interactionsUniqueLayersCurrent << " unique\n";
        }
        o << "\n--- Inventory / stats ---\n";
        if (attch.playerInventoryItems > 0) {
            o << "Player inventory      : " << attch.playerInventoryItems << " items\n";
        }
        if (attch.playerStashItems > 0) {
            o << "Player stash          : " << attch.playerStashItems << " items\n";
        }
        if (attch.statsBundles > 0) {
            o << "Stats bundles         : " << attch.statsBundles << "\n";
        }
        o << "\n--- GPU / streaming ---\n";
        if (attch.gpuUsedMemoryMB > 0 || attch.gpuTotalMemoryMB > 0) {
            o << "GPU memory used       : " << attch.gpuUsedMemoryMB
              << " / " << attch.gpuTotalMemoryMB << " MB\n";
        }
        if (!attch.streamingMountState.empty()) {
            o << "Streaming mount state : " << attch.streamingMountState << "\n";
        }
        if (!attch.streamingLastObserverPosition.empty()) {
            o << "Last observer pos     : " << attch.streamingLastObserverPosition << "\n";
        }
        if (!attch.engineVersionWatermark.empty()) {
            o << "\n--- Engine build ---\n";
            o << attch.engineVersionWatermark << "\n";
        }
    }

    if (meta.hasData) {
        o << "\n--- Save metadata ---\n";
        if (!meta.saveName.empty()) {
            o << "Save name             : " << meta.saveName << "\n";
        }
        if (!meta.buildID.empty()) {
            o << "Save buildID          : " << meta.buildID << "\n";
        }
        if (!meta.initialBuildID.empty()) {
            o << "Save initialBuildID   : " << meta.initialBuildID;
            if (!meta.buildID.empty() && meta.initialBuildID != meta.buildID) {
                o << "\n";
                o << "                        <-- CROSS-VERSION SAVE: this save was\n";
                o << "                            CREATED on initialBuildID but is being\n";
                o << "                            loaded on buildID. Known AV source.\n";
            } else {
                o << "\n";
            }
        }
        if (meta.saveVersion > 0) {
            o << "Save version          : " << meta.saveVersion;
            if (meta.gameVersion > 0) o << "  (gameVersion " << meta.gameVersion << ")";
            o << "\n";
        }
        if (meta.isModdedSet) {
            o << "Save was modded       : " << (meta.isModded ? "true" : "false") << "\n";
        }
        if (!meta.trackedQuest.empty()) {
            o << "Tracked quest         : " << meta.trackedQuest << "\n";
        }
        if (!meta.locationName.empty()) {
            o << "Location (locKey)     : " << meta.locationName << "\n";
        }
        if (meta.playerLevel > 0) {
            o << "Player level          : " << meta.playerLevel << "\n";
        }
        if (meta.playTime > 0.0) {
            o << "Play time (seconds)   : " << (uint64_t)meta.playTime << "\n";
        }
        if (!meta.lifePath.empty()) {
            o << "Lifepath              : " << meta.lifePath << "\n";
        }
        if (!meta.difficulty.empty()) {
            o << "Difficulty            : " << meta.difficulty << "\n";
        }
    }

    return o.str();
}

uint32_t EnrichRecentCrashes(const std::filesystem::path& crashesDir,
                             const std::filesystem::path& reportQueueRoot) {
    std::error_code ec;
    if (!std::filesystem::exists(crashesDir, ec)) return 0;
    if (!std::filesystem::exists(reportQueueRoot, ec)) return 0;

    uint32_t written = 0;
    for (auto const& entry : std::filesystem::directory_iterator(crashesDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto path = entry.path();
        if (path.extension() != ".crash") continue;

        std::filesystem::path sidecar = path;
        sidecar.replace_extension(".engine-state.txt");
        if (std::filesystem::exists(sidecar, ec)) continue;

        auto info = ParseCrashHeader(path);
        if (!info.ok) continue;

        auto rqDir = FindReportQueueDir(reportQueueRoot, info.pid, info.crashTime);
        if (rqDir.empty()) continue;

        auto attchPath = FindAttchText(rqDir);
        auto metaPath  = FindSaveMetadataJson(rqDir);
        EngineDumpAttchFields attch{};
        SaveMetadataFields    meta{};
        if (!attchPath.empty()) attch = ParseEngineDumpAttchText(SlurpFile(attchPath));
        if (!metaPath.empty())  meta  = ParseSaveMetadataJson(SlurpFile(metaPath));

        auto text = RenderEngineStateSidecar(attch, meta);
        std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
        if (!out) continue;
        out.write(text.data(), (std::streamsize)text.size());
        if (out.good()) ++written;
    }
    return written;
}

uint32_t EnrichRecentCrashesDefaultRoot(const std::filesystem::path& crashesDir) {
    char localAppData[MAX_PATH] = {};
    size_t got = 0;
    if (getenv_s(&got, localAppData, sizeof(localAppData), "LOCALAPPDATA") != 0 ||
        got == 0) return 0;
    std::filesystem::path root = std::filesystem::path(localAppData)
        / "REDEngine" / "ReportQueue";
    return EnrichRecentCrashes(crashesDir, root);
}

}
