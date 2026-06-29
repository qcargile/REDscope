#include "Config.h"
#include <fstream>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace redscope {
namespace {

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool ParseBool(const std::string& v, bool def) {
    auto lower = v;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") return true;
    if (lower == "false"|| lower == "0" || lower == "no"  || lower == "off") return false;
    return def;
}

uint32_t ParseUint(const std::string& v, uint32_t def) {
    try {
        for (char c : v) {
            if (c == '-') return def;
            if (!std::isspace((unsigned char)c)) break;
        }
        return static_cast<uint32_t>(std::stoul(v));
    }
    catch (...) { return def; }
}

uint32_t ParseUintClamped(const std::string& v, uint32_t def, uint32_t lo, uint32_t hi) {
    uint32_t parsed = ParseUint(v, def);
    if (parsed < lo) return lo;
    if (parsed > hi) return hi;
    return parsed;
}

}

Config LoadConfig(const std::filesystem::path& pluginDir) {
    Config cfg;
    cfg.symbolDbPath = pluginDir / "symbols" / "Cyberpunk2077.symbols.json";

    auto iniPath = pluginDir / "REDscope.ini";
    std::ifstream in(iniPath);
    if (!in.is_open()) return cfg;

    std::unordered_map<std::string, std::string> kv;
    std::string line, section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = Trim(line.substr(0, eq));
        auto val = Trim(line.substr(eq + 1));
        kv[section + "." + key] = val;
    }

    auto get = [&](const char* k) -> const std::string* {
        auto it = kv.find(k); return it == kv.end() ? nullptr : &it->second;
    };

    if (auto v = get("core.respect_debugger"))                cfg.respectDebugger = ParseBool(*v, cfg.respectDebugger);
    if (auto v = get("core.terminate_process_after_report"))  cfg.terminateProcessAfterReport = ParseBool(*v, cfg.terminateProcessAfterReport);
    if (auto v = get("core.capture_all_thread_stacks"))       cfg.captureAllThreadStacks = ParseBool(*v, cfg.captureAllThreadStacks);
    if (auto v = get("core.max_crash_files"))                 cfg.maxCrashFiles = ParseUintClamped(*v, cfg.maxCrashFiles, 1, 10000);

    if (auto v = get("buffers.find_entity_ring_size"))        cfg.findEntityRingSize   = ParseUintClamped(*v, cfg.findEntityRingSize, 4, 4096);
    if (auto v = get("buffers.tweakdb_ring_size"))            cfg.tweakDbRingSize      = ParseUintClamped(*v, cfg.tweakDbRingSize, 4, 4096);
    if (auto v = get("buffers.archive_ring_size"))            cfg.archiveRingSize      = ParseUintClamped(*v, cfg.archiveRingSize, 4, 4096);
    if (auto v = get("buffers.snapshot_interval_ms"))         cfg.snapshotIntervalMs   = ParseUintClamped(*v, cfg.snapshotIntervalMs, 50, 60000);

    if (auto v = get("symbols.symbol_db_path")) {
        std::filesystem::path p(*v);
        cfg.symbolDbPath = p.is_absolute() ? p : pluginDir / p;
    }
    if (auto v = get("symbols.auto_download")) cfg.symbolsAutoDownload = ParseBool(*v, cfg.symbolsAutoDownload);

    if (auto v = get("stack.dump_slot_count"))                cfg.stackDumpSlotCount   = ParseUintClamped(*v, cfg.stackDumpSlotCount, 1, 65536);

    if (auto v = get("test.test_inject_crash_after_seconds")) cfg.testInjectCrashAfterSeconds = ParseUint(*v, 0);

    if (auto v = get("watchdog.hang_watchdog_enabled"))  cfg.hangWatchdogEnabled  = ParseBool(*v, cfg.hangWatchdogEnabled);
    if (auto v = get("watchdog.hang_threshold_seconds")) cfg.hangThresholdSeconds = ParseUint(*v, cfg.hangThresholdSeconds);

    return cfg;
}

}
