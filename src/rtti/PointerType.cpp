#include "PointerType.h"
#include "HashDecoder.h"
#include "../util/SehGuardedRead.h"
#include "../util/FixedStr.h"
#include "../snapshot/Snapshot.h"
#include "../snapshot/SnapshotWorker.h"
#include "../symbols/SymbolDb.h"
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <cctype>
#include <cstring>

namespace redscope::rtti {

bool MatchSentinelLabel(uint64_t value, char* outLabel, size_t cap) noexcept {
    if (!outLabel || cap < 2) return false;
    auto copy = [&](const char* s) {
        size_t i = 0;
        while (i + 1 < cap && s[i]) { outLabel[i] = s[i]; ++i; }
        outLabel[i] = '\0';
    };
    switch (value) {
        case 0xFFFFFFFFFFFFFFFFull: copy("INVALID / NOT_FOUND (-1 i64)"); return true;
        case 0xFFFFFFFFFFFFFFFEull: copy("sentinel (-2 i64)");            return true;
        case 0x7FFFFFFFFFFFFFFFull: copy("INT64_MAX");                    return true;
        case 0x8000000000000000ull: copy("INT64_MIN");                    return true;
    }
    if ((value >> 32) == 0) {
        uint32_t v32 = (uint32_t)value;
        switch (v32) {
            case 0xFFFFFFFFu: copy("INVALID / NOT_FOUND (-1 i32)"); return true;
            case 0xFFFFFFFEu: copy("sentinel (-2 i32)");            return true;
            case 0x7FFFFFFFu: copy("INT32_MAX");                    return true;
            case 0x80000000u: copy("INT32_MIN");                    return true;
        }
    }
    return false;
}

namespace {

bool DemangleRttiName(const char* mangled, char* out, size_t cap) noexcept {
    if (!mangled || !out || cap < 2) return false;
    const char* start = mangled[0] == '.' ? mangled + 1 : mangled;
    if (start[0] != '?') return false;

    char buf[256] = {};
    DWORD len = ::UnDecorateSymbolName(start, buf, (DWORD)sizeof(buf),
                                       UNDNAME_NAME_ONLY | UNDNAME_NO_ARGUMENTS);
    if (len == 0 || buf[0] == '\0') return false;

    char* p = buf;
    while (*p == ' ' || *p == '\t') ++p;
    size_t slen = std::strlen(p);
    while (slen > 0 && (p[slen - 1] == ' ' || p[slen - 1] == '\t' ||
                        p[slen - 1] == '\r' || p[slen - 1] == '\n')) {
        p[--slen] = '\0';
    }
    if (slen == 0) return false;

    if (std::strncmp(p, "class ",  6) == 0)  { p += 6;  slen -= 6;  }
    else if (std::strncmp(p, "struct ", 7) == 0) { p += 7;  slen -= 7;  }
    else if (std::strncmp(p, "union ",  6) == 0) { p += 6;  slen -= 6;  }

    if (slen >= cap) slen = cap - 1;
    std::memcpy(out, p, slen);
    out[slen] = '\0';
    return true;
}

void CrudeStripRttiName(const char* mangled, char* out, size_t cap) noexcept {
    if (!mangled || !out || cap < 2) { if (out && cap) out[0] = '\0'; return; }
    const char* start = std::strstr(mangled, "AV");
    if (!start) start = std::strstr(mangled, "AU");
    if (!start) {
        size_t n = std::strlen(mangled);
        if (n >= cap) n = cap - 1;
        std::memcpy(out, mangled, n);
        out[n] = '\0';
        return;
    }
    start += 2;
    const char* end = std::strstr(start, "@@");
    size_t n = end ? (size_t)(end - start) : std::strlen(start);
    if (n >= cap) n = cap - 1;
    std::memcpy(out, start, n);
    out[n] = '\0';
}

bool WalkMsvcRtti(uintptr_t objPtr,
                  const Snapshot& snap,
                  const ModuleInfoFixed& mod,
                  char* outName,
                  size_t outCap) noexcept {
    if (!outName || outCap < 2) return false;
    outName[0] = '\0';

    uintptr_t vtableAddr = 0;
    if (!SehSafeReadValue(vtableAddr, reinterpret_cast<const void*>(objPtr))) return false;

    if (vtableAddr < mod.base || vtableAddr >= mod.base + mod.size) return false;

    uintptr_t colPtr = 0;
    if (!SehSafeReadValue(colPtr, reinterpret_cast<const void*>(vtableAddr - 8))) return false;
    if (colPtr < mod.base || colPtr >= mod.base + mod.size) return false;

    struct ColHeader { uint32_t signature; uint32_t offset; uint32_t cdOffset; uint32_t tdRva; } col{};
    if (!SehSafeReadValue(col, reinterpret_cast<const void*>(colPtr))) return false;
    if (col.signature != 1) return false;

    uintptr_t tdAddr = mod.base + col.tdRva;
    if (tdAddr < mod.base || tdAddr >= mod.base + mod.size) return false;

    char mangled[192] = {};
    if (!SehSafeRead(mangled, reinterpret_cast<const void*>(tdAddr + 0x10), sizeof(mangled) - 1)) {
        return false;
    }
    mangled[sizeof(mangled) - 1] = '\0';
    if (mangled[0] == '\0') return false;

    if (!DemangleRttiName(mangled, outName, outCap)) {
        CrudeStripRttiName(mangled, outName, outCap);
    }
    return outName[0] != '\0';
}

size_t DetectAsciiString(uintptr_t addr, char* out, size_t outCap,
                         size_t maxLen = 128) noexcept {
    if (!out || outCap < 2) return 0;
    out[0] = '\0';
    if (!LooksReadable(reinterpret_cast<const void*>(addr), 1)) return 0;

    char scratch[256] = {};
    if (maxLen > sizeof(scratch)) maxLen = sizeof(scratch);
    if (!SehSafeRead(scratch, reinterpret_cast<const void*>(addr), maxLen)) return 0;

    size_t i = 0;
    for (; i < maxLen; ++i) {
        char c = scratch[i];
        if (c == '\0') break;
        const unsigned char uc = (unsigned char)c;
        const bool printable = (uc >= 0x20 && uc <= 0x7E) || c == '\t' || c == '\n' || c == '\r';
        if (!printable) return 0;
    }
    if (i == 0 || i >= maxLen) return 0;

    size_t copyN = i < outCap - 1 ? i : outCap - 1;
    std::memcpy(out, scratch, copyN);
    out[copyN] = '\0';
    return i;
}

bool DetectExecutableSection(uintptr_t ownerBase, uintptr_t addr,
                             PointerInfo& info) noexcept {
    const uint8_t* ownerBytes = reinterpret_cast<const uint8_t*>(ownerBase);
    IMAGE_DOS_HEADER dos{};
    if (!SehSafeReadValue(dos, ownerBytes) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!SehSafeReadValue(nt, ownerBytes + dos.e_lfanew) || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        ownerBytes + dos.e_lfanew + sizeof(IMAGE_NT_HEADERS64));
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i, ++sec) {
        IMAGE_SECTION_HEADER s{};
        if (!SehSafeReadValue(s, sec)) break;
        auto begin = ownerBase + s.VirtualAddress;
        auto end   = begin + s.Misc.VirtualSize;
        if (addr >= begin && addr < end &&
            (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            info.kind = PointerKind::Code;
            uint32_t symOff = 0;
            if (redscope::symbols::ResolveCodeAddress(info.moduleName, addr,
                                                      (uint32_t)info.moduleOffset,
                                                      info.symbolName,
                                                      kPointerInfoSymbolCap,
                                                      symOff)) {
                info.symbolOffset = symOff;
            }
            return true;
        }
    }
    return false;
}

}

bool TryGetObjectClassName(uintptr_t addr, char* out, size_t cap) noexcept {
    if (out && cap) out[0] = '\0';
    if (addr < 0x10000 || !out || cap < 2) return false;
    if (!LooksReadable(reinterpret_cast<void*>(addr), 8)) return false;

    uintptr_t vtbl = 0;
    if (!SehSafeReadValue(vtbl, reinterpret_cast<const void*>(addr)) || vtbl == 0) return false;

    const Snapshot* snap = redscope::snap::Current();
    if (!snap) return false;

    if (auto* mf = redscope::FindModuleByPC(*snap, vtbl)) {
        if (WalkMsvcRtti(addr, *snap, *mf, out, cap)) return true;
    }

    uint64_t nativeType = 0;
    if (SehSafeReadValue(nativeType, reinterpret_cast<const void*>(addr + 0x30)) && nativeType) {
        uint64_t nameHash = 0;
        if (SehSafeReadValue(nameHash, reinterpret_cast<const void*>(nativeType + 0x18))) {
            auto dec = TryDecodeCName(nameHash);
            if (dec.decoded) {
                redscope::CopyFixed(out, cap, dec.text);
                return out[0] != '\0';
            }
        }
    }
    return false;
}

PointerInfo Inspect(uintptr_t addr) noexcept {
    PointerInfo info;
    if (addr == 0) { info.kind = PointerKind::Null; return info; }
    {
        char label[kPointerInfoNameCap] = {};
        if (MatchSentinelLabel(static_cast<uint64_t>(addr), label, sizeof(label))) {
            info.kind = PointerKind::Sentinel;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, label);
            return info;
        }
    }
    if (!LooksReadable(reinterpret_cast<void*>(addr), 8)) return info;

    uintptr_t ownerBase = 0;
    const Snapshot* snap = redscope::snap::Current();
    if (snap) {
        if (auto* mf = redscope::FindModuleByPC(*snap, addr)) {
            redscope::CopyFixed(info.moduleName, kPointerInfoNameCap,
                                snap->moduleNames[mf->nameIndex]);
            info.moduleOffset = addr - mf->base;
            ownerBase         = mf->base;
        }
    } else {
        HMODULE live = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(addr), &live) && live) {
            char path[MAX_PATH] = {};
            if (::GetModuleFileNameA(live, path, MAX_PATH) == 0) path[0] = 0;
            const char* name = std::strrchr(path, '\\');
            redscope::CopyFixed(info.moduleName, kPointerInfoNameCap,
                                name ? name + 1 : path);
            info.moduleOffset = addr - reinterpret_cast<uintptr_t>(live);
            ownerBase         = reinterpret_cast<uintptr_t>(live);
        }
    }

    if (ownerBase && DetectExecutableSection(ownerBase, addr, info)) return info;

    {
        char clsName[kPointerInfoNameCap] = {};
        if (TryGetObjectClassName(addr, clsName, sizeof(clsName))) {
            info.kind = PointerKind::RttiObject;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, clsName);
            return info;
        }
    }

    {
        auto dec = TryDecodeCName(static_cast<uint64_t>(addr));
        if (dec.decoded) {
            info.kind = PointerKind::CName;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, dec.text);
            return info;
        }
    }

    {
        auto dec = TryDecodeTweakDBID(static_cast<uint64_t>(addr));
        if (dec.decoded) {
            info.kind = PointerKind::TweakDBID;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, dec.text);
            return info;
        }
    }

    {
        char strBuf[kPointerInfoNameCap] = {};
        if (DetectAsciiString(addr, strBuf, sizeof(strBuf)) > 0) {
            info.kind = PointerKind::String;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, strBuf);
            return info;
        }
    }

    if (ownerBase) {
        info.kind = PointerKind::OtherData;
    }
    return info;
}

PointerInfo InspectFast(uintptr_t addr) noexcept {
    PointerInfo info;
    if (addr == 0) { info.kind = PointerKind::Null; return info; }
    {
        char label[kPointerInfoNameCap] = {};
        if (MatchSentinelLabel(static_cast<uint64_t>(addr), label, sizeof(label))) {
            info.kind = PointerKind::Sentinel;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, label);
            return info;
        }
    }
    if (!LooksReadable(reinterpret_cast<void*>(addr), 8)) return info;

    uintptr_t ownerBase = 0;
    const Snapshot* snap = redscope::snap::Current();
    if (snap) {
        if (auto* mf = redscope::FindModuleByPC(*snap, addr)) {
            redscope::CopyFixed(info.moduleName, kPointerInfoNameCap,
                                snap->moduleNames[mf->nameIndex]);
            info.moduleOffset = addr - mf->base;
            ownerBase         = mf->base;
        }
    }

    if (ownerBase && DetectExecutableSection(ownerBase, addr, info)) return info;

    {
        auto dec = TryDecodeCName(static_cast<uint64_t>(addr));
        if (dec.decoded) {
            info.kind = PointerKind::CName;
            redscope::CopyFixed(info.className, kPointerInfoNameCap, dec.text);
            return info;
        }
    }

    if (ownerBase) {
        info.kind = PointerKind::OtherData;
    }
    return info;
}

}
