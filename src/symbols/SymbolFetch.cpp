#include "SymbolFetch.h"
#include "../Logger.h"

#include <windows.h>
#include <winhttp.h>
#include <simdjson.h>
#include <miniz.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cwctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

namespace redscope::symfetch {
namespace {

constexpr size_t kMaxZipBytes = 256u * 1024 * 1024;
constexpr size_t kMaxApiBytes = 8u * 1024 * 1024;
constexpr uint32_t kMaxPdbDirBytes = 64u * 1024 * 1024;

std::atomic<bool> s_inFlight{false};

struct Target {
    const wchar_t* moduleName;
    const char*    repo;
    const char*    label;
};

const Target kTargets[] = {
    { L"RED4ext.dll",             "WopsS/RED4ext",                          "RED4ext" },
    { L"cyber_engine_tweaks.asi", "maximegmd/CyberEngineTweaks",            "CET" },
    { L"mod_settings.dll",        "jackhumbert/mod_settings",               "Mod Settings" },
    { L"audioware.dll",           "cyb3rpsych0s1s/audioware",               "Audioware" },
    { L"input_loader.dll",        "jackhumbert/cyberpunk2077-input-loader", "Input Loader" },
};

void LogInfo(const std::string& s) { redscope::log::Info("[symbols] " + s); }
void LogWarn(const std::string& s) { redscope::log::Warn("[symbols] " + s); }

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

struct PdbId {
    GUID     guid{};
    uint32_t age = 0;
    bool     valid = false;
};

bool SameId(const PdbId& a, const PdbId& b) {
    return a.valid && b.valid && a.age == b.age && std::memcmp(&a.guid, &b.guid, sizeof(GUID)) == 0;
}

std::vector<char> ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

PdbId ReadDllCodeView(const std::wstring& dllPath) {
    PdbId id;
    auto b = ReadFileBytes(dllPath);
    if (b.size() < sizeof(IMAGE_DOS_HEADER)) return id;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(b.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return id;
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > b.size()) return id;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(b.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return id;
    auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dd.VirtualAddress || !dd.Size) return id;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    if ((const char*)(sec + nt->FileHeader.NumberOfSections) > b.data() + b.size()) return id;

    auto rvaToOff = [&](DWORD rva) -> DWORD {
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            DWORD va = sec[i].VirtualAddress;
            DWORD vs = sec[i].Misc.VirtualSize;
            if (rva >= va && rva < va + vs) return sec[i].PointerToRawData + (rva - va);
        }
        return 0;
    };
    DWORD dbgOff = rvaToOff(dd.VirtualAddress);
    if (!dbgOff || (size_t)dbgOff + dd.Size > b.size()) return id;
    int count = (int)(dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY));
    auto* dbg = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(b.data() + dbgOff);
    for (int i = 0; i < count; ++i) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        DWORD cvOff = dbg[i].PointerToRawData;
        if (!cvOff || (size_t)cvOff + 24 > b.size()) continue;
        const char* cv = b.data() + cvOff;
        if (std::memcmp(cv, "RSDS", 4) != 0) continue;
        std::memcpy(&id.guid, cv + 4, 16);
        std::memcpy(&id.age, cv + 20, 4);
        id.valid = true;
        return id;
    }
    return id;
}

PdbId ParsePdbId(const char* data, size_t size) {
    PdbId id;
    static const char kMagic[] = "Microsoft C/C++ MSF 7.00\r\n" "\x1a" "DS";
    const size_t magicLen = 29;
    if (size < 64 || std::memcmp(data, kMagic, magicLen) != 0) return id;

    auto rd32 = [&](size_t off) -> uint32_t {
        if (off + 4 > size) return 0;
        uint32_t v = 0; std::memcpy(&v, data + off, 4); return v;
    };
    uint32_t blockSize    = rd32(32);
    uint32_t numDirBytes  = rd32(44);
    uint32_t blockMapAddr = rd32(52);
    if (blockSize < 64 || blockSize > size) return id;
    if (numDirBytes == 0 || numDirBytes > size || numDirBytes > kMaxPdbDirBytes) return id;

    auto blockPtr = [&](uint32_t blk, size_t need) -> const char* {
        size_t off = (size_t)blk * blockSize;
        if (off + need > size) return nullptr;
        return data + off;
    };

    uint32_t numDirBlocks = (numDirBytes + blockSize - 1) / blockSize;
    const char* mapBlk = blockPtr(blockMapAddr, (size_t)numDirBlocks * 4);
    if (!mapBlk) return id;

    std::vector<char> dir; dir.reserve(numDirBytes);
    for (uint32_t i = 0; i < numDirBlocks && dir.size() < numDirBytes; ++i) {
        uint32_t blk = 0; std::memcpy(&blk, mapBlk + (size_t)i * 4, 4);
        const char* db = blockPtr(blk, blockSize);
        if (!db) return id;
        uint32_t take = numDirBytes - (uint32_t)dir.size();
        if (take > blockSize) take = blockSize;
        dir.insert(dir.end(), db, db + take);
    }
    if (dir.size() < 8) return id;

    auto d32 = [&](size_t off) -> uint32_t {
        if (off + 4 > dir.size()) return 0xFFFFFFFFu;
        uint32_t v = 0; std::memcpy(&v, dir.data() + off, 4); return v;
    };
    size_t pos = 0;
    uint32_t numStreams = d32(pos); pos += 4;
    if (numStreams < 2 || numStreams > 1000000) return id;
    if (pos + (size_t)numStreams * 4 > dir.size()) return id;
    uint32_t size0 = d32(pos);
    uint32_t size1 = d32(pos + 4);
    pos += (size_t)numStreams * 4;

    auto blocksFor = [&](uint32_t sz) -> uint32_t {
        return (sz == 0xFFFFFFFFu) ? 0 : (sz + blockSize - 1) / blockSize;
    };
    pos += (size_t)blocksFor(size0) * 4;
    if (size1 == 0 || size1 == 0xFFFFFFFFu) return id;
    uint32_t s1first = d32(pos);
    const char* s1 = blockPtr(s1first, 28);
    if (!s1) return id;

    std::memcpy(&id.age, s1 + 8, 4);
    std::memcpy(&id.guid, s1 + 12, 16);
    id.valid = true;
    return id;
}

PdbId ReadPdbId(const std::wstring& pdbPath) {
    auto b = ReadFileBytes(pdbPath);
    if (b.empty()) return {};
    return ParsePdbId(b.data(), b.size());
}

std::string ModuleFileVersion(HMODULE mod) {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return "";
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return "";
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return "";
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !ffi) return "";
    DWORD ms = ffi->dwFileVersionMS, ls = ffi->dwFileVersionLS;
    char v[64];
    std::snprintf(v, sizeof(v), "%u.%u.%u", HIWORD(ms), LOWORD(ms), HIWORD(ls));
    return v;
}

bool IsGithubAssetUrl(const std::wstring& url) {
    URL_COMPONENTS uc; ZeroMemory(&uc, sizeof(uc)); uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    std::wstring h(host);
    for (auto& c : h) c = (wchar_t)std::towlower(c);
    if (h == L"github.com") return true;
    static const wchar_t* suffix = L".githubusercontent.com";
    size_t n = wcslen(suffix);
    return h.size() >= n && h.compare(h.size() - n, n, suffix) == 0;
}

bool HttpGet(const std::wstring& url, std::string& out, const wchar_t* accept, size_t maxBytes) {
    out.clear();
    URL_COMPONENTS uc; ZeroMemory(&uc, sizeof(uc)); uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}; wchar_t path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"REDscope",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    bool ok = false;
    bool overflow = false;
    if (HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0)) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
            std::wstring headers = L"User-Agent: REDscope\r\n";
            if (accept) { headers += L"Accept: "; headers += accept; headers += L"\r\n"; }
            if (WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1L,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, NULL)) {
                DWORD status = 0, slen = sizeof(status);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
                        if (maxBytes && out.size() + avail > maxBytes) { overflow = true; break; }
                        size_t old = out.size();
                        out.resize(old + avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(hReq, &out[old], avail, &read)) { out.resize(old); break; }
                        out.resize(old + read);
                    }
                    ok = !overflow && !out.empty();
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    if (overflow) out.clear();
    return ok;
}

bool FindSymbolAsset(const std::string& releaseJson, std::string& urlOut) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(simdjson::padded_string(releaseJson)).get(doc) != simdjson::SUCCESS) return false;
    simdjson::dom::array assets;
    if (doc["assets"].get(assets) != simdjson::SUCCESS) return false;
    for (auto a : assets) {
        std::string_view nm, url;
        if (a["name"].get(nm) != simdjson::SUCCESS) continue;
        if (a["browser_download_url"].get(url) != simdjson::SUCCESS) continue;
        std::string low = ToLower(std::string(nm));
        bool isSym = low.find("pdb") != std::string::npos || low.find("symbol") != std::string::npos;
        bool isDev = low.find("dev_only") != std::string::npos || low.find("dev-only") != std::string::npos;
        if (isSym && !isDev) { urlOut.assign(url); return true; }
    }
    return false;
}

bool WriteBytes(const std::filesystem::path& dest, const void* data, size_t size) {
    std::ofstream of(dest, std::ios::binary);
    if (!of) return false;
    of.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
    return (bool)of;
}

void FetchOne(const Target& t, const std::filesystem::path& symbolsDir) {
    HMODULE mod = GetModuleHandleW(t.moduleName);
    if (!mod) return;

    wchar_t dllPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, dllPath, MAX_PATH)) return;
    PdbId want = ReadDllCodeView(dllPath);
    if (!want.valid) { LogWarn(std::string(t.label) + ": could not read its debug id"); return; }

    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(symbolsDir, ec)) {
        if (e.path().extension() == L".pdb" && SameId(ReadPdbId(e.path().wstring()), want)) {
            LogInfo(std::string(t.label) + ": symbols already present");
            return;
        }
    }

    std::string ver = ModuleFileVersion(mod);
    std::vector<std::string> tags;
    if (!ver.empty()) { tags.push_back("v" + ver); tags.push_back(ver); }
    tags.push_back("__latest__");

    for (const auto& tag : tags) {
        std::string apiPath = std::string("/repos/") + t.repo +
            (tag == "__latest__" ? std::string("/releases/latest")
                                 : std::string("/releases/tags/") + tag);
        std::string apiUrlA = "https://api.github.com" + apiPath;
        std::string json;
        if (!HttpGet(std::wstring(apiUrlA.begin(), apiUrlA.end()), json, L"application/vnd.github+json", kMaxApiBytes)) continue;

        std::string assetUrl;
        if (!FindSymbolAsset(json, assetUrl)) continue;
        std::wstring wAssetUrl(assetUrl.begin(), assetUrl.end());
        if (!IsGithubAssetUrl(wAssetUrl)) { LogWarn(std::string(t.label) + ": asset url not a github host, skipped"); continue; }

        std::string zipBytes;
        if (!HttpGet(wAssetUrl, zipBytes, nullptr, kMaxZipBytes)) continue;

        bool placedAny = false;
        mz_zip_archive zip; std::memset(&zip, 0, sizeof(zip));
        if (mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
            mz_uint n = mz_zip_reader_get_num_files(&zip);
            for (mz_uint i = 0; i < n; ++i) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
                std::string fn = st.m_filename;
                std::string low = ToLower(fn);
                if (low.size() < 4 || low.substr(low.size() - 4) != ".pdb") continue;
                size_t slash = fn.find_last_of("/\\");
                std::string base = (slash == std::string::npos) ? fn : fn.substr(slash + 1);
                if (base.empty()) continue;

                size_t outSize = 0;
                void* mem = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
                if (!mem) continue;
                PdbId got = ParsePdbId(reinterpret_cast<const char*>(mem), outSize);
                auto dest = symbolsDir / base;
                if (got.valid && !SameId(got, want)) {
                    LogWarn(std::string(t.label) + ": " + base + " build id mismatch (" + tag + "), skipped");
                } else if (WriteBytes(dest, mem, outSize)) {
                    placedAny = true;
                    LogInfo(std::string(t.label) + ": placed " + base + " (" + tag + ")");
                }
                mz_free(mem);
            }
            mz_zip_reader_end(&zip);
        }
        if (placedAny) return;
    }
    LogWarn(std::string(t.label) + ": no matching symbols found online");
}

void RunAll(const std::filesystem::path& symbolsDir) {
    std::error_code ec;
    std::filesystem::create_directories(symbolsDir, ec);
    LogInfo("auto-download starting");
    for (const auto& t : kTargets) {
        try { FetchOne(t, symbolsDir); }
        catch (...) { LogWarn(std::string(t.label) + ": fetch threw, skipped"); }
    }
    LogInfo("auto-download done");
}

void RunGuarded(const std::filesystem::path& symbolsDir) {
    bool expected = false;
    if (!s_inFlight.compare_exchange_strong(expected, true)) {
        LogInfo("download already in progress, skipped");
        return;
    }
    try { RunAll(symbolsDir); } catch (...) {}
    s_inFlight.store(false);
}

}

void RunDownloadBlocking(const std::filesystem::path& symbolsDir) {
    RunGuarded(symbolsDir);
}

void StartAutoDownloadAsync(const std::filesystem::path& symbolsDir) {
    std::filesystem::path dir = symbolsDir;
    std::thread([dir]() { RunGuarded(dir); }).detach();
}

}
