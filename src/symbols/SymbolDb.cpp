#include "SymbolDb.h"
#include "../Logger.h"
#include <simdjson.h>
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace redscope::symbols {
namespace {

std::vector<uint32_t> g_rva;
std::vector<uint32_t> g_symbolStart;
std::vector<uint32_t> g_symbolLen;
std::vector<char>     g_symbolBlob;

std::atomic<bool>        g_ready{false};
std::atomic<const char*> g_status{"idle"};
std::atomic<bool>        g_loadStarted{false};

uint32_t ParseHex32(std::string_view s) {
    char buf[32];
    size_t n = s.size() < sizeof(buf) - 1 ? s.size() : sizeof(buf) - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return (uint32_t)std::strtoul(buf, nullptr, 16);
}

uint64_t ParseHex64(std::string_view s) {
    char buf[32];
    size_t n = s.size() < sizeof(buf) - 1 ? s.size() : sizeof(buf) - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return std::strtoull(buf, nullptr, 16);
}

uint32_t ParseSectionOffset(std::string_view s) {
    size_t colon = s.find(':');
    if (colon == std::string_view::npos) return 0;
    return ParseHex32(s.substr(colon + 1));
}

void LoadThread(std::filesystem::path jsonPath) {
    g_status.store("loading", std::memory_order_release);

    std::string narrowPath = jsonPath.string();

    try {
        auto t0 = std::chrono::steady_clock::now();

        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string::load(narrowPath);
        simdjson::ondemand::document doc = parser.iterate(json);

        std::vector<uint32_t> rvas;
        std::vector<uint32_t> starts;
        std::vector<uint32_t> lens;
        rvas.reserve(1'000'000);
        starts.reserve(1'000'000);
        lens.reserve(1'000'000);

        std::vector<char> blob;
        blob.reserve(64 * 1024 * 1024);

        uint64_t preferredBase = 0;
        uint32_t codeConst     = 0;
        bool addressesSeen = false;
        simdjson::ondemand::object top = doc.get_object();
        for (auto field : top) {
            std::string_view key = field.unescaped_key();
            if (key == "Preferred load address") {
                std::string_view v = field.value().get_string();
                preferredBase = ParseHex64(v);
            } else if (key == "Code constant offset") {
                std::string_view v = field.value().get_string();
                codeConst = ParseHex32(v);
            } else if (key == "Addresses") {
                addressesSeen = true;
                for (auto entry : field.value().get_array()) {
                    std::string_view symbolStr;
                    std::string_view offsetStr;
                    bool haveSymbol = false;
                    bool haveOffset = false;
                    for (auto efield : entry.get_object()) {
                        std::string_view ekey = efield.unescaped_key();
                        if (ekey == "symbol") {
                            symbolStr = efield.value().get_string();
                            haveSymbol = true;
                        } else if (ekey == "offset") {
                            offsetStr = efield.value().get_string();
                            haveOffset = true;
                        }
                    }
                    if (!haveSymbol || !haveOffset) continue;

                    uint32_t sectOff = ParseSectionOffset(offsetStr);

                    if (blob.size() + symbolStr.size() > UINT32_MAX) {
                        g_status.store("parse_error", std::memory_order_release);
                        redscope::log::Warn("SymbolDb: symbol blob exceeds 4GB; aborting load");
                        return;
                    }

                    uint32_t startIdx = (uint32_t)blob.size();
                    uint32_t len = (uint32_t)symbolStr.size();
                    blob.insert(blob.end(), symbolStr.begin(), symbolStr.end());

                    rvas.push_back(sectOff);
                    starts.push_back(startIdx);
                    lens.push_back(len);
                }
            }
        }

        if (!addressesSeen) {
            g_status.store("parse_error", std::memory_order_release);
            redscope::log::Warn("SymbolDb: parse failed: 'Addresses' field missing");
            return;
        }

        for (auto& r : rvas) r += codeConst;

        std::vector<uint32_t> order(rvas.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return rvas[a] < rvas[b]; });

        std::vector<uint32_t> sortedRva;
        std::vector<uint32_t> sortedStart;
        std::vector<uint32_t> sortedLen;
        sortedRva.reserve(rvas.size() + 1);
        sortedStart.reserve(rvas.size() + 1);
        sortedLen.reserve(rvas.size() + 1);
        for (uint32_t i : order) {
            sortedRva.push_back(rvas[i]);
            sortedStart.push_back(starts[i]);
            sortedLen.push_back(lens[i]);
        }

        sortedRva.push_back(0xFFFFFFFFu);
        sortedStart.push_back((uint32_t)blob.size());
        sortedLen.push_back(0);

        g_rva         = std::move(sortedRva);
        g_symbolStart = std::move(sortedStart);
        g_symbolLen   = std::move(sortedLen);
        g_symbolBlob  = std::move(blob);

        g_ready.store(true, std::memory_order_release);
        g_status.store("ready", std::memory_order_release);

        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        HMODULE exe = ::GetModuleHandleW(L"Cyberpunk2077.exe");
        uint64_t actualBase = exe ? (uint64_t)exe : 0;

        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "SymbolDb: %zu entries, %zu KB blob, parsed in %lld ms "
            "(preferred=0x%llX actual=0x%llX codeConst=0x%X)",
            g_rva.size() - 1,
            g_symbolBlob.size() / 1024,
            (long long)dt,
            (unsigned long long)preferredBase,
            (unsigned long long)actualBase,
            codeConst);
        redscope::log::Info(msg);
    } catch (const simdjson::simdjson_error& e) {
        g_status.store("parse_error", std::memory_order_release);
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "SymbolDb: parse failed for %s: %s",
            narrowPath.c_str(), e.what());
        redscope::log::Warn(msg);
    } catch (...) {
        g_status.store("parse_error", std::memory_order_release);
        redscope::log::Warn("SymbolDb: parse failed with unknown exception");
    }
}

}

void StartLoad(const std::filesystem::path& gameRoot) {
    bool expected = false;
    if (!g_loadStarted.compare_exchange_strong(expected, true)) return;

    std::filesystem::path jsonPath = gameRoot / L"bin" / L"x64" / L"cyberpunk2077_addresses.json";
    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec)) {
        g_status.store("missing", std::memory_order_release);
        return;
    }

    std::thread(LoadThread, std::move(jsonPath)).detach();
}

bool Ready() noexcept {
    return g_ready.load(std::memory_order_acquire);
}

LookupResult Lookup(uint32_t rvaInModule) noexcept {
    LookupResult r;
    if (!Ready()) return r;

    auto it = std::lower_bound(g_rva.begin(), g_rva.end(), rvaInModule);
    if (it != g_rva.end() && *it == rvaInModule) {
        size_t idx = (size_t)(it - g_rva.begin());
        if (g_symbolLen[idx] == 0) return r;
        r.found = true;
        r.symbol = std::string_view(g_symbolBlob.data() + g_symbolStart[idx], g_symbolLen[idx]);
        r.offsetIntoFunction = 0;
        return r;
    }
    if (it == g_rva.begin()) return r;
    --it;
    size_t idx = (size_t)(it - g_rva.begin());
    if (g_symbolLen[idx] == 0) return r;
    r.found = true;
    r.symbol = std::string_view(g_symbolBlob.data() + g_symbolStart[idx], g_symbolLen[idx]);
    r.offsetIntoFunction = rvaInModule - g_rva[idx];
    return r;
}

size_t EntryCount() noexcept {
    if (!Ready()) return 0;
    return g_rva.size() - 1;
}

const char* Status() noexcept {
    return g_status.load(std::memory_order_acquire);
}

bool LookupByAddress(uintptr_t absoluteVa, char* nameOut, size_t nameCap,
                     uint32_t& offsetOut) noexcept {
    offsetOut = 0;
    if (nameOut && nameCap >= 1) nameOut[0] = '\0';
    if (!nameOut || nameCap < 2 || absoluteVa == 0) return false;

    constexpr DWORD kMaxNameLen = 384;
    alignas(SYMBOL_INFO) uint8_t storage[sizeof(SYMBOL_INFO) + kMaxNameLen * sizeof(char)] = {};
    PSYMBOL_INFO sym = reinterpret_cast<PSYMBOL_INFO>(storage);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = kMaxNameLen;

    DWORD64 displacement = 0;
    BOOL ok = FALSE;
    __try {
        ok = ::SymFromAddr(::GetCurrentProcess(),
                           static_cast<DWORD64>(absoluteVa),
                           &displacement, sym);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!ok || sym->Name[0] == '\0') return false;

    const char* src = sym->Name;
    char demangled[384];
    if (src[0] == '?' || src[0] == '_') {
        DWORD dlen = ::UnDecorateSymbolName(src, demangled, (DWORD)sizeof(demangled),
                                            UNDNAME_NAME_ONLY | UNDNAME_NO_ARGUMENTS);
        if (dlen > 0 && demangled[0] != '\0') src = demangled;
    }

    size_t n = std::strlen(src);
    if (n >= nameCap) n = nameCap - 1;
    std::memcpy(nameOut, src, n);
    nameOut[n] = '\0';
    offsetOut = static_cast<uint32_t>(displacement);
    return true;
}

bool ResolveCodeAddress(const char* moduleName, uintptr_t absoluteVa, uint32_t rva,
                        char* nameOut, size_t nameCap, uint32_t& offsetOut,
                        bool* approximate) noexcept {
    if (approximate) *approximate = false;
    if (!nameOut || nameCap < 2) return false;
    nameOut[0] = '\0';
    offsetOut = 0;

    if (moduleName && std::strcmp(moduleName, "Cyberpunk2077.exe") == 0 && Ready()) {
        auto s = Lookup(rva);
        if (s.found && s.offsetIntoFunction < 0x10000u) {
            size_t n = s.symbol.size();
            if (n >= nameCap) n = nameCap - 1;
            std::memcpy(nameOut, s.symbol.data(), n);
            nameOut[n] = '\0';
            offsetOut = s.offsetIntoFunction;
            if (approximate) *approximate = (s.offsetIntoFunction >= 0x1000u); // nearest of a sparse named set, no extents
            return true;
        }
        return false;
    }

    if (!LookupByAddress(absoluteVa, nameOut, nameCap, offsetOut)) return false;
    if (offsetOut >= 0x10000u) {
        nameOut[0] = '\0';
        offsetOut = 0;
        return false;
    }
    return true;
}

}
