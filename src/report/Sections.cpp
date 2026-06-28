#include "Sections.h"
#include "CrashClass.h"
#include "CulpritHeuristic.h"
#include "Fingerprint.h"
#include "JsonSidecar.h"
#include "ResourceAtFault.h"
#include "../Logger.h"
#include "../Plugin.h"
#include "../breadcrumbs/Breadcrumb.h"
#include "../breadcrumbs/BreadcrumbStore.h"
#include "../hooks/ScriptStack.h"
#include "../logs/LogTailer.h"
#include "../rtti/PointerType.h"
#include "../rtti/ObjectsInFlight.h"
#include "../rtti/FaultInstruction.h"
#include "../util/SehGuardedRead.h"
#include "../snapshot/InstalledMods.h"
#include "../snapshot/Snapshot.h"
#include "../snapshot/SnapshotWorker.h"
#include "../symbols/SymbolDb.h"
#include "../util/StringUtils.h"
#include <dbghelp.h>
#include <psapi.h>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace redscope::report {

static const char* ExceptionCodeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_INVALID_HANDLE:           return "INVALID_HANDLE";
        case 0xC0000374:                         return "HEAP_CORRUPTION";
        case 0xC0000409:                         return "STACK_BUFFER_OVERRUN";
        case 0xC0000420:                         return "ASSERTION_FAILURE";
        case 0x40010005:                         return "DBG_CONTROL_C";
        case 0x40010008:                         return "DBG_CONTROL_BREAK";
        case 0x406D1388:                         return "MS_VC_THREAD_NAME";
        case 0xE06D7363:                         return "CXX_EXCEPTION";
        case 0xE0434352:                         return "CLR_EXCEPTION";
        case 0xE24C4A02:                         return "CET_SHUTDOWN";
        case 0xDEAD0001:                         return "REDSCOPE_NESTED_CRASH";
        default:                                 return "UNKNOWN";
    }
}

void EmitHeader(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep, DWORD threadId) {
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    auto addr  = ep ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    using clock = std::chrono::system_clock;
    auto now = clock::to_time_t(clock::now());
    char timeBuf[64]; std::tm tmv{}; localtime_s(&tmv, &now);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmv);

    out.Append("================================================================================\n");
    out.Append(" REDscope Crash Report\n");
    out.Append("================================================================================\n");

    const Snapshot* snapshot = redscope::snap::Current();
    const char* symbolName = nullptr;
    char symbolBuf[256] = {};
    if (snapshot && addr) {
        if (auto* mf = redscope::FindModuleByPC(*snapshot, (uintptr_t)addr)) {
            const char* name = snapshot->moduleNames[mf->nameIndex];
            uint64_t off = (uint64_t)addr - (uint64_t)mf->base;
            uint32_t symOff = 0;
            if (redscope::symbols::ResolveCodeAddress(name, (uintptr_t)addr, (uint32_t)off,
                                                     symbolBuf, sizeof(symbolBuf), symOff)) {
                symbolName = symbolBuf;
            }
        }
    }

    redscope::rtti::InFlightSet inFlight;
    if (ep && ep->ContextRecord) {
        inFlight = redscope::rtti::DecodeObjectsInFlight(*ep->ContextRecord);
    }

    redscope::report::ResourceAtFaultResult resAtFault;
    if (ep && ep->ContextRecord && code != EXCEPTION_STACK_OVERFLOW) {        // SO path runs on a depleted stack; skip the 512-slot scan
        resAtFault = redscope::report::DecodeResourceAtFault(*ep->ContextRecord, snapshot);
    }
    redscope::report::SetLastResourceAtFault(resAtFault);

    auto verdict = redscope::report::ComputeVerdict(code, (uintptr_t)addr,
                                                    snapshot, symbolName, &inFlight, &resAtFault);
    redscope::report::EmitCulpritLine(out, verdict);

    auto fingerprint = redscope::report::ComputeFingerprint(code, (uintptr_t)addr,
                                                            snapshot, symbolName);
    redscope::report::SetLastFingerprint(fingerprint);
    redscope::report::EmitCrashId(out, fingerprint);

    out.Append("--- Crash ---------------------------------------------------------------------\n");
    out.Appendf("Exception : 0x%08lX %s\n", code, ExceptionCodeName(code));
    out.Appendf("Address   : 0x%p\n", addr);
    if (ep && ep->ExceptionRecord &&
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const auto* einfo = ep->ExceptionRecord->ExceptionInformation;
        const char* op = einfo[0] == 0 ? "read"
                       : einfo[0] == 1 ? "write"
                       : einfo[0] == 8 ? "execute"
                       : "access";
        const uintptr_t dataAddr = (uintptr_t)einfo[1];
        out.Appendf("Fault data: %s at 0x%016llX\n", op,
                    (unsigned long long)dataAddr);

        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T got = 0;
        __try {
            got = ::VirtualQuery(reinterpret_cast<LPCVOID>(dataAddr),
                                 &mbi, sizeof(mbi));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            got = 0;
        }
        if (got == sizeof(mbi)) {
            const char* state =
                mbi.State == MEM_FREE    ? "FREE"
              : mbi.State == MEM_RESERVE ? "RESERVE"
              : mbi.State == MEM_COMMIT  ? "COMMIT"
              : "?";
            const char* type =
                mbi.State == MEM_FREE        ? ""
              : mbi.Type  == MEM_IMAGE       ? " IMAGE"
              : mbi.Type  == MEM_MAPPED      ? " MAPPED"
              : mbi.Type  == MEM_PRIVATE     ? " PRIVATE"
              : "";
            char protBuf[32] = "";
            if (mbi.State == MEM_COMMIT) {
                DWORD p = mbi.Protect;
                const char* prot =
                    (p & PAGE_NOACCESS)          ? "NOACCESS"
                  : (p & PAGE_EXECUTE_READWRITE) ? "EXECUTE_READWRITE"
                  : (p & PAGE_EXECUTE_READ)      ? "EXECUTE_READ"
                  : (p & PAGE_EXECUTE)           ? "EXECUTE"
                  : (p & PAGE_READWRITE)         ? "READWRITE"
                  : (p & PAGE_READONLY)          ? "READONLY"
                  : "?";
                std::snprintf(protBuf, sizeof(protBuf), " %s", prot);
                if (p & PAGE_GUARD)          std::strcat(protBuf, "+GUARD");
            }
            const char* hint =
                mbi.State == MEM_FREE                          ? "  -> unmapped; dangling ptr / garbage"
              : mbi.State == MEM_RESERVE                       ? "  -> reserved VM; alloc overran commit"
              : (mbi.State == MEM_COMMIT &&
                 (mbi.Protect & PAGE_NOACCESS))                ? "  -> freed page or guard; use-after-free"
              : (mbi.State == MEM_COMMIT &&
                 mbi.Type  == MEM_IMAGE)                       ? "  -> inside a loaded module; ptr corruption"
              : (mbi.State == MEM_COMMIT &&
                 (mbi.Protect & (PAGE_READWRITE|PAGE_READONLY)))? "  -> live region; struct field / overrun"
              : "";
            out.Appendf("            region: %s%s%s%s\n",
                        state, type, protBuf, hint);

            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE) {
                HMODULE mh = nullptr;
                ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(dataAddr), &mh);
                if (mh) {
                    char path[MAX_PATH] = {};
                    ::GetModuleFileNameA(mh, path, MAX_PATH);
                    const char* nm = std::strrchr(path, '\\');
                    out.Appendf("            module: %s + 0x%llX\n",
                                nm ? nm + 1 : path,
                                (unsigned long long)(dataAddr - (uintptr_t)mh));
                }
            }
        }

        bool interpDone = false;
        if (ep->ContextRecord) {
            uint8_t insn[16] = {};
            if (redscope::SehSafeRead(insn, reinterpret_cast<const void*>(ep->ContextRecord->Rip), sizeof(insn))) {
                redscope::rtti::DecodedFault df =
                    redscope::rtti::DecodeFaultOperand(insn, sizeof(insn), *ep->ContextRecord, dataAddr);
                if (df.ok && df.eaMatches) {
                    if (df.baseIsNull) {
                        out.Appendf("            interpret: null-pointer %s - operand %s, %s = 0 (field +0x%llX)\n",
                                    op, df.operand, df.baseReg, (unsigned long long)df.disp);
                    } else if (df.baseHasReg) {
                        out.Appendf("            interpret: %s %s - %s = 0x%llX (bad/dangling pointer)\n",
                                    op, df.operand, df.baseReg, (unsigned long long)df.baseValue);
                    } else {
                        out.Appendf("            interpret: %s %s\n", op, df.operand);
                    }
                    interpDone = true;
                }
            }
        }
        if (!interpDone && ep->ContextRecord) {
            char interp[256] = {};
            if (redscope::rtti::InterpretNullDeref(*ep->ContextRecord, dataAddr, op, interp, sizeof(interp))) {
                out.Appendf("            interpret: %s\n", interp);
            }
        }
    }
    out.Appendf("Thread    : %lu\n", threadId);
    out.Appendf("Time      : %s\n", timeBuf);
    {
        char dumpDir[200] = {};
        std::tm tmv2{};
        localtime_s(&tmv2, &now);
        char dateBuf[16], timeB[16];
        std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", &tmv2);
        std::strftime(timeB,   sizeof(timeB),   "%H%M%S", &tmv2);
        std::snprintf(dumpDir, sizeof(dumpDir),
                      "%%LOCALAPPDATA%%\\REDEngine\\ReportQueue\\"
                      "Cyberpunk2077-%s-%s-%lu-*",
                      dateBuf, timeB, (unsigned long)::GetCurrentProcessId());
        out.Appendf("Engine dump: %s\n", dumpDir);
    }
    out.Appendf("Class     : %s\n", CrashClassLabel(CurrentCrashClass()));
    out.Appendf("REDscope  : %s\n", REDSCOPE_VERSION);
    const Snapshot* s = redscope::snap::Current();
    if (s) {
        out.Appendf("RED4ext   : %s\n",
                    s->red4extVersion[0] ? s->red4extVersion : "(unknown)");
        out.Appendf("Cyberpunk : %s\n",
                    s->cp2077BuildString[0] ? s->cp2077BuildString : "(unknown)");
    } else {
        out.Append("RED4ext   : (snapshot not yet available)\n");
        out.Append("Cyberpunk : (snapshot not yet available)\n");
    }
    out.Append("\n");
}

void EmitBreadcrumbs(PreallocatedBuffer& out) {
    out.Append("--- Breadcrumbs (last 32) -----------------------------------------------------\n");
    auto& store = redscope::GetBreadcrumbStore();
    int64_t crashNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    constexpr size_t kMax = 32;
    redscope::Breadcrumb buf[kMax] = {};
    size_t writeIdx = 0;
    size_t totalSeen = 0;
    store.ring.Snapshot([&](const redscope::Breadcrumb& b) {
        buf[writeIdx] = b;
        writeIdx = (writeIdx + 1) % kMax;
        ++totalSeen;
    });

    if (totalSeen == 0) { out.Append("(none)\n\n"); return; }

    const size_t count = (totalSeen < kMax) ? totalSeen : kMax;
    const size_t startIdx = (totalSeen < kMax) ? 0 : writeIdx;
    for (size_t i = 0; i < count; ++i) {
        const auto& b = buf[(startIdx + i) % kMax];
        double secsRelToCrash = (b.timestampNs - crashNs) / 1e9;
        out.Appendf("[%+07.3fs] [%-16.*s] %.*s\n",
                    secsRelToCrash,
                    (int)redscope::kBreadcrumbTagLen, b.tag,
                    (int)redscope::kBreadcrumbMsgLen, b.message);
    }
    out.Append("\n");
}

void EmitProcessMemory(PreallocatedBuffer& out) {
    out.Append("--- Process memory ------------------------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) {
        out.Append("(snapshot not yet available)\n\n");
        return;
    }
    const uint64_t mb = 1024ull * 1024ull;
    out.Appendf("Working set : %llu MB\n", (unsigned long long)(s->memory.workingSetBytes / mb));
    out.Appendf("Commit      : %llu MB\n", (unsigned long long)(s->memory.commitBytes / mb));
    out.Appendf("Virtual     : %llu MB\n", (unsigned long long)(s->memory.virtualBytes / mb));
    out.Appendf("Handles     : %u\n", s->memory.handleCount);
    out.Append("\n");
}

void EmitEngineState(PreallocatedBuffer& out) {
    out.Append("--- Engine state (at crash) ---------------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s || !s->engineLive.hasData) {
        out.Append("(engine state not yet sampled)\n\n");
        return;
    }
    const auto& e = s->engineLive;
    if (e.engineReadOk) {
        out.Appendf("Engine state    : %s\n", redscope::snap::EngineStateName(e.engineStateRaw));
        out.Appendf("Scripts loaded  : %s\n", e.scriptsLoaded ? "true" : "false");
        out.Appendf("Engine closing  : %s\n", e.isClosing ? "true  (crash during shutdown)" : "false");
        out.Appendf("Phantom Liberty : %s\n", e.isEP1 ? "true" : "false");
    } else {
        out.Append("Engine flags    : (unavailable)\n");
    }
    if (e.gpuReadOk) {
        out.Appendf("GPU memory      : %u / %u MB used (budget %u MB)\n",
                    e.gpuUsedMB, e.gpuDedicatedMB, e.gpuBudgetMB);
    } else {
        out.Append("GPU memory      : (unavailable)\n");
    }
    if (e.oomSuspected) {
        out.Appendf("OOM suspected   : true  <-- %s\n", e.oomBasis);
    } else {
        out.Append("OOM suspected   : false\n");
    }
    out.Append("\n");
}

void EmitRecentLogs(PreallocatedBuffer& out,
                    std::chrono::system_clock::time_point crashTime) {
    out.Append("--- Recent log activity (60s window around crash) -----------------------------\n");

    const wchar_t* root = redscope::snap::GameRoot();
    if (!root || root[0] == L'\0') {
        out.Append("(game root not yet resolved)\n\n");
        return;
    }

    struct Source {
        const char* label;
        const char* relativeDir;
        const char* glob;
        bool        timestamped;
    };
    static const Source sources[] = {
        { "red4ext",       "red4ext/logs",                             "red4ext-*.log",         true  },
        { "redscript",     "r6/logs",                                  "redscript_rCURRENT.log",true  },
        { "CET",           "bin/x64/plugins/cyber_engine_tweaks",      "cyber_engine_tweaks.log", false },
        { "ArchiveXL",     "red4ext/plugins/ArchiveXL",                "ArchiveXL-*.log",       true  },
        { "TweakXL",       "red4ext/plugins/TweakXL",                  "TweakXL-*.log",         true  },
        { "audioware",     "red4ext/logs",                             "audioware-*.log",       true  },
        { "Codeware",      "red4ext/plugins/Codeware",                 "Codeware-*.log",        true  },
        { "input_loader",  "red4ext/logs",                             "input_loader-*.log",    true  },
        { "mod_settings",  "red4ext/logs",                             "mod_settings-*.log",    true  },
    };

    struct PerSource {
        const Source*            src      = nullptr;
        std::filesystem::path    file;
        std::vector<std::string> lines;
    };
    std::vector<PerSource> collected;
    collected.reserve(std::size(sources));

    for (const auto& src : sources) {
        std::filesystem::path dir = std::filesystem::path(root) / src.relativeDir;
        std::filesystem::path file = redscope::logs::FindNewestMatching(dir, src.glob);
        if (file.empty()) continue;

        auto lines = src.timestamped
            ? redscope::logs::WindowAround(file, crashTime, 60, 30)
            : redscope::logs::Tail(file, 30);
        if (lines.empty()) continue;

        collected.push_back({ &src, std::move(file), std::move(lines) });
    }

    struct ErrorHit { const char* label; const std::string* line; int rank; };
    std::vector<ErrorHit> hits;
    hits.reserve(32);
    auto rankLine = [](const std::string& s) -> int {
        static const char* kHigh[] = {
            "[error]", "[ERROR]", "[ERR]", "[fatal]", "[FATAL]",
            "FATAL:", "PANIC",
            "Failed to load", "failed to load",
            "Access violation", "access violation",
            "Exception",
        };
        static const char* kMed[] = {
            "[warn]", "[WARN]", "[warning]", "[WARNING]",
        };
        for (const char* n : kHigh) {
            if (s.find(n) != std::string::npos) return 2;
        }
        for (const char* n : kMed) {
            if (s.find(n) != std::string::npos) return 1;
        }
        return 0;
    };
    for (const auto& ps : collected) {
        for (const auto& ln : ps.lines) {
            int r = rankLine(ln);
            if (r > 0) hits.push_back({ ps.src->label, &ln, r });
        }
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const ErrorHit& a, const ErrorHit& b) { return a.rank > b.rank; });

    constexpr size_t kMaxLineLen = 240;
    auto appendLine = [&](const char* indent, const std::string& ln) {
        if (ln.size() <= kMaxLineLen) {
            out.Appendf("%s%s\n", indent, ln.c_str());
        } else {
            out.Appendf("%s%.*s ... [truncated]\n", indent, (int)kMaxLineLen, ln.c_str());
        }
    };

    if (!hits.empty()) {
        const size_t kMaxHoisted = 20;
        size_t shown = hits.size() < kMaxHoisted ? hits.size() : kMaxHoisted;
        out.Appendf("\nErrors / warnings in window (%zu):\n", hits.size());
        for (size_t i = 0; i < shown; ++i) {
            out.Appendf("  [%s] ", hits[i].label);
            appendLine("", *hits[i].line);
        }
        if (shown < hits.size()) {
            out.Appendf("  ... %zu more not shown (full logs on disk)\n",
                        hits.size() - shown);
        }
    }

    if (hits.empty()) {
        out.Append(collected.empty()
            ? "(no log files found within window)\n"
            : "(no errors or warnings in window)\n");
    }
    out.Append("\n");
}

static void EmitFrame(PreallocatedBuffer& out, int idx, DWORD64 pc, const char* tag) {
    const char* name = "?";
    DWORD64 off = 0;
    char modPath[MAX_PATH] = {};
    const Snapshot* s = redscope::snap::Current();
    if (s) {
        if (auto* mf = redscope::FindModuleByPC(*s, (uintptr_t)pc)) {
            name = s->moduleNames[mf->nameIndex];
            off  = pc - mf->base;
        }
    } else {
        HMODULE mod = nullptr;
        ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(pc), &mod);
        if (mod) {
            if (::GetModuleFileNameExA(::GetCurrentProcess(), mod, modPath, MAX_PATH) == 0) modPath[0] = 0;
        }
        const char* slash = std::strrchr(modPath, '\\');
        name = slash ? slash + 1 : (modPath[0] ? modPath : "?");
        off  = mod ? pc - reinterpret_cast<uintptr_t>(mod) : 0;
    }
    char symName[256] = {};
    uint32_t symOff = 0;
    bool symApprox = false;
    if (redscope::symbols::ResolveCodeAddress(name, (uintptr_t)pc, (uint32_t)off,
                                              symName, sizeof(symName), symOff, &symApprox)) {
        const char* pfx = symApprox ? "~" : "";
        const char* sfx = symApprox ? "  (nearest sym)" : "";
        if (tag) out.Appendf("[%2d] 0x%016llX  %s!%s%s+0x%X  %s%s\n",
                             idx, pc, name, pfx, symName, symOff, tag, sfx);
        else     out.Appendf("[%2d] 0x%016llX  %s!%s%s+0x%X%s\n",
                             idx, pc, name, pfx, symName, symOff, sfx);
        return;
    }
    if (tag) out.Appendf("[%2d] 0x%016llX  %s + 0x%llX  %s\n", idx, pc, name, off, tag);
    else     out.Appendf("[%2d] 0x%016llX  %s + 0x%llX\n",     idx, pc, name, off);
}

void EmitNativeCallstack(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep) {
    out.Append("--- Native call stack ---------------------------------------------------------\n");
    if (!ep) { out.Append("(no exception context)\n\n"); return; }

    HANDLE proc = ::GetCurrentProcess();
    HANDLE thrd = ::GetCurrentThread();

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;

    EmitFrame(out, 0, ctx.Rip, "<faulting RIP>");

    int emitted = 1;
    for (int i = 0; emitted < 64; ++i) {
        if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thrd, &frame, &ctx,
                           nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr)) break;
        if (frame.AddrPC.Offset == 0) break;
        if (i == 0 && frame.AddrPC.Offset == ctx.Rip) continue;
        EmitFrame(out, emitted, frame.AddrPC.Offset, nullptr);
        ++emitted;
    }
    out.Append("\n");
}

void EmitLastActive(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep) {
    out.Append("--- LAST ACTIVE ---------------------------------------------------------------\n");

    const Snapshot* snap = redscope::snap::Current();

    {
        out.Append("Scripted code (per thread):\n");
        const redscope::scriptstack::HeartbeatSlot* slots = nullptr;
        size_t count = 0;
        redscope::scriptstack::GetHeartbeatSlots(&slots, &count);

        const DWORD crashingTid = ::GetCurrentThreadId();
        const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        uint32_t populated = 0;
        if (slots) {
            for (size_t i = 0; i < count; ++i) if (slots[i].threadId != 0) ++populated;
        }

        if (!slots || populated == 0) {
            out.Append("  (no thread has entered a scripted frame this session)\n");
        } else {
            const auto& scriptNow = redscope::scriptstack::Current();
            const bool noScriptInFlight = (scriptNow.depth == 0 && scriptNow.overflow == 0);
            auto renderRow = [&](const redscope::scriptstack::HeartbeatSlot& s) {
                const char* name = s.lastFullName ? s.lastFullName : "(unnamed)";
                const double secsAgo = s.lastEnteredNs > 0
                    ? (nowNs - s.lastEnteredNs) / 1e9 : 0.0;
                const bool isCrashing = (s.threadId == crashingTid);
                const char* note = (isCrashing && noScriptInFlight)
                    ? "   <- last scripted fn on this thread; NOT in flight at crash (native)"
                    : "";
                out.Appendf("  %s tid=%-6u  %-56s  (%+07.3fs ago)%s\n",
                            isCrashing ? "[CRASHING]" : "          ",
                            (unsigned)s.threadId, name, -secsAgo, note);
            };
            bool crashingShown = false;
            for (size_t i = 0; i < count; ++i) {
                if (slots[i].threadId == crashingTid) {
                    renderRow(slots[i]);
                    crashingShown = true;
                    break;
                }
            }
            if (!crashingShown) {
                out.Appendf("  [CRASHING] tid=%-6lu  (never entered a scripted frame this session)\n",
                            (unsigned long)crashingTid);
            }
            for (size_t i = 0; i < count; ++i) {
                if (slots[i].threadId == 0) continue;
                if (slots[i].threadId == crashingTid) continue;
                renderRow(slots[i]);
            }
        }
        out.Append("\n");
    }

    {
        out.Append("Mod code on native stack (crashing thread):\n");
        ModStackFrame modFrames[8] = {};
        size_t nMod = GatherModFramesOnStack(ep, snap, modFrames,
                                             sizeof(modFrames) / sizeof(modFrames[0]));
        if (nMod == 0) {
            out.Append("  (none - native stack contains no frames in installed mod DLLs)\n");
        } else {
            for (size_t i = 0; i < nMod; ++i) {
                const auto& f = modFrames[i];
                out.Appendf("  [%2d] 0x%016llX  %s + 0x%llX\n",
                            f.frameIndex,
                            (unsigned long long)f.pc,
                            f.moduleName ? f.moduleName : "?",
                            (unsigned long long)(f.pc - f.moduleBase));
            }
        }
        out.Append("\n");
    }

    {
        out.Append("Objects on the crashing stack (RTTI):\n");
        redscope::rtti::InFlightSet stackObjs;
        if (ep && ep->ContextRecord) {
            stackObjs = redscope::rtti::DecodeObjectsOnStack(*ep->ContextRecord);
        }
        redscope::rtti::SetLastStackObjects(stackObjs);
        if (stackObjs.count == 0) {
            out.Append("  (no RTTI-typed objects found on the captured stack window)\n");
        } else {
            for (uint32_t i = 0; i < stackObjs.count; ++i) {
                const auto& o = stackObjs.items[i];
                if (o.modFields > 0) {
                    out.Appendf("  %s  [SP+0x%X, +%u mod field%s]\n", o.className,
                                o.stackOffset, o.modFields, o.modFields == 1 ? "" : "s");
                } else {
                    out.Appendf("  %s  [SP+0x%X]\n", o.className, o.stackOffset);
                }
            }
        }
        out.Append("\n");
    }

    {
        out.Append("Live game state (pushed via REDscope.SetState):\n");
        const auto& g = snap ? snap->gameStateLive : redscope::snap::GameStateLive{};
        if (!snap || !g.hasData || g.count == 0) {
            out.Append("  (no live state pushed this session)\n");
        } else {
            for (uint32_t i = 0; i < g.count; ++i) {
                const auto& e = g.entries[i];
                if (e.key[0] == '\0') continue;
                out.Appendf("  %-20s  %s\n", e.key, e.value);
            }
        }
        out.Append("\n");
    }

    {
        out.Append("Mods changed since last launch:\n");
        if (!snap) {
            out.Append("  (snapshot unavailable)\n");
        } else {
            const auto& d = snap->modDiff;
            if (!d.priorFilePresent) {
                out.Append("  (first session since REDscope install; no baseline recorded)\n");
            } else if (d.added.empty() && d.removed.empty() && d.updated.empty()) {
                out.Append("  (no changes)\n");
            } else {
                out.Appendf("  +%zu added / -%zu removed / ~%zu updated "
                            "(full list under 'Modlist changes since last launch')\n",
                            d.added.size(), d.removed.size(), d.updated.size());
            }
        }
        out.Append("\n");
    }
}

void EmitScriptCallstack(PreallocatedBuffer& out) {
    const auto& s = redscope::scriptstack::Current();

    if (s.depth == 0 && s.overflow == 0) {
        out.Append("--- RedScript call stack (crashing thread) ------------------------------------\n");
        out.Append("(no scripted frames in flight)\n\n");
        return;
    }

    out.Appendf("--- RedScript call stack (crashing thread, depth %u) ---------------------------\n",
                (unsigned)(s.depth + s.overflow));

    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    for (uint32_t i = 0; i < s.depth; ++i) {
        const auto& f = s.frames[i];
        double secsAgo = (nowNs - f.enteredNs) / 1e9;
        const char* name = f.fullName ? f.fullName : "(unnamed)";
        const char* suffix = "";
        if (i == 0 && s.overflow == 0 && s.depth > 1)               suffix = "  <- outermost";
        else if (i == s.depth - 1 && s.overflow == 0)               suffix = "  <- innermost (executing)";
        char labeled[160];
        const char* shown = name;
        if (f.thisClassName && f.thisClassName[0]) {
            int written = std::snprintf(labeled, sizeof(labeled), "%s on %s", name, f.thisClassName);
            if (written > 0) shown = labeled;
        }
        out.Appendf("[%2u] %-56s  (%+07.3fs ago)%s\n",
                    (unsigned)i, shown, -secsAgo, suffix);
    }

    if (s.overflow > 0) {
        out.Appendf("... %u additional frames dropped (depth cap = %zu)\n",
                    (unsigned)s.overflow, redscope::scriptstack::kMaxDepth);
    }
    if (s.maxDepthSeen > s.depth + s.overflow) {
        out.Appendf("Max depth seen this session: %u\n", (unsigned)s.maxDepthSeen);
    }
    out.Append("\n");
}

void EmitRegisters(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep) {
    out.Append("--- Registers -----------------------------------------------------------------\n");
    if (!ep) { out.Append("(no exception context)\n\n"); return; }
    const CONTEXT& c = *ep->ContextRecord;

    struct Reg { const char* name; uintptr_t value; };
    Reg regs[] = {
        {"RAX", c.Rax}, {"RBX", c.Rbx}, {"RCX", c.Rcx}, {"RDX", c.Rdx},
        {"RSI", c.Rsi}, {"RDI", c.Rdi}, {"RBP", c.Rbp}, {"RSP", c.Rsp},
        {"R8 ", c.R8 }, {"R9 ", c.R9 }, {"R10", c.R10}, {"R11", c.R11},
        {"R12", c.R12}, {"R13", c.R13}, {"R14", c.R14}, {"R15", c.R15},
        {"RIP", c.Rip},
    };
    for (const auto& r : regs) {
        auto info = redscope::rtti::Inspect(r.value);
        switch (info.kind) {
            case redscope::rtti::PointerKind::Null:
                out.Appendf("%s  0x%016llX   NULL\n", r.name, (unsigned long long)r.value);
                break;
            case redscope::rtti::PointerKind::Code:
                if (info.symbolName[0]) {
                    out.Appendf("%s  0x%016llX   (Code) %s!%s+0x%X\n",
                                r.name, (unsigned long long)r.value,
                                info.moduleName, info.symbolName, info.symbolOffset);
                } else {
                    out.Appendf("%s  0x%016llX   (Code) %s + 0x%llX\n",
                                r.name, (unsigned long long)r.value,
                                info.moduleName, (unsigned long long)info.moduleOffset);
                }
                break;
            case redscope::rtti::PointerKind::RttiObject: {
                const Snapshot* s2 = redscope::snap::Current();
                const redscope::snap::ClassWithScriptedFields* fc =
                    s2 ? redscope::snap::FindScriptedFieldClass(
                             s2->rttiSnapshot, info.className) : nullptr;
                const char* label = "Object";
                if (fc) {
                    out.Appendf("%s  0x%016llX   (%s) %s  [+%u mod fields]\n",
                                r.name, (unsigned long long)r.value,
                                label, info.className, fc->scriptedFieldCount);
                } else {
                    out.Appendf("%s  0x%016llX   (%s) %s\n",
                                r.name, (unsigned long long)r.value,
                                label, info.className);
                }
                break;
            }
            case redscope::rtti::PointerKind::String:
                out.Appendf("%s  0x%016llX   (String) \"%s\"\n",
                            r.name, (unsigned long long)r.value, info.className);
                break;
            case redscope::rtti::PointerKind::CName:
                out.Appendf("%s  0x%016llX   CName(%s)\n",
                            r.name, (unsigned long long)r.value, info.className);
                break;
            case redscope::rtti::PointerKind::TweakDBID:
                out.Appendf("%s  0x%016llX   %s\n",
                            r.name, (unsigned long long)r.value, info.className);
                break;
            case redscope::rtti::PointerKind::Sentinel:
                out.Appendf("%s  0x%016llX   %s\n",
                            r.name, (unsigned long long)r.value, info.className);
                break;
            case redscope::rtti::PointerKind::OtherData:
                out.Appendf("%s  0x%016llX   (data in %s + 0x%llX)\n",
                            r.name, (unsigned long long)r.value,
                            info.moduleName[0] ? info.moduleName : "?",
                            (unsigned long long)info.moduleOffset);
                break;
            case redscope::rtti::PointerKind::Unknown:
                out.Appendf("%s  0x%016llX\n", r.name, (unsigned long long)r.value);
                break;
        }
    }
    out.Append("\n");
}

static uint32_t CountKind(const Snapshot& s, ModuleKind kind) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s.moduleCount; ++i) {
        if (s.modules[i].kind == kind) ++n;
    }
    return n;
}

static const snap::PluginInfo* LookupPlugin(const Snapshot& s, const char* moduleName) {
    char key[redscope::kModuleNameCap];
    size_t n = LowerToAscii(moduleName, key, sizeof(key));
    auto it = s.pluginMeta.byDll.find(std::string_view(key, n));
    return it == s.pluginMeta.byDll.end() ? nullptr : &it->second;
}

static void EmitGroup(PreallocatedBuffer& out, const Snapshot& s,
                      ModuleKind kind, const char* label) {
    uint32_t count = CountKind(s, kind);
    out.Appendf("%s (%u):\n", label, count);
    if (count == 0) { out.Append("  (none)\n\n"); return; }
    for (uint32_t i = 0; i < s.moduleCount; ++i) {
        const ModuleInfoFixed& m = s.modules[i];
        if (m.kind != kind) continue;
        const char* name = s.moduleNames[m.nameIndex];
        const snap::PluginInfo* meta = kind == ModuleKind::Mod ? LookupPlugin(s, name) : nullptr;
        if (meta && !meta->authors.empty()) {
            out.Appendf("  0x%016llX  %-32s  v%-12.*s  by %.*s\n",
                        (unsigned long long)m.base,
                        name,
                        (int)meta->version.size(), meta->version.data(),
                        (int)meta->authors.size(), meta->authors.data());
        } else if (meta) {
            out.Appendf("  0x%016llX  %-32s  v%-12.*s\n",
                        (unsigned long long)m.base,
                        name,
                        (int)meta->version.size(), meta->version.data());
        } else {
            out.Appendf("  0x%016llX  %-32s  v%-12s\n",
                        (unsigned long long)m.base,
                        name,
                        s.moduleVersions[m.versionIndex]);
        }
    }
    out.Append("\n");
}

void EmitWrapChains(PreallocatedBuffer& out) {
    out.Append("--- Wrap chains ---------------------------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s || !s->wrapChains.scanPerformed) {
        out.Append("(wrap chain scan not yet complete)\n\n");
        return;
    }
    const auto& tbl = s->wrapChains;
    const uint32_t chainCount = (uint32_t)tbl.chains.size();
    out.Appendf("Discovered %u wrap chain%s across %u mod%s on disk (%u .reds files scanned).\n",
                chainCount, chainCount == 1 ? "" : "s",
                tbl.modsWithWraps, tbl.modsWithWraps == 1 ? "" : "s",
                tbl.filesScanned);
    if (tbl.chainsDropped > 0) {
        out.Appendf("%u additional chains discovered but dropped (cap = %zu).\n",
                    tbl.chainsDropped, redscope::snap::kMaxWrapChains);
    }
    if (chainCount == 0) {
        out.Append("\n");
        return;
    }

    const auto& stack = redscope::scriptstack::Current();
    uint32_t matched = 0;
    if (stack.depth > 0) {
        out.Append("\nChains for methods in scripted call stack:\n");
        for (uint32_t i = 0; i < stack.depth; ++i) {
            const char* full = stack.frames[i].fullName;
            if (!full) continue;

            char key[redscope::snap::kWrapMethodKeyCap] = {};
            redscope::snap::NormalizeWrapMethodKey(full, key, sizeof(key));

            const auto* chain = redscope::snap::FindChain(tbl, key);
            if (!chain) continue;

            ++matched;
            out.Appendf("\n  %s (%u layer%s, inferred from alphabetical script load order):\n",
                        chain->methodKey,
                        chain->layerCount,
                        chain->layerCount == 1 ? "" : "s");
            for (uint32_t k = 0; k < chain->layerCount; ++k) {
                const auto& l = chain->layers[k];
                out.Appendf("    [%u] %-28s %s:%u\n",
                            k + 1,
                            l.modName[0] ? l.modName : "(unknown)",
                            l.relFile[0] ? l.relFile : "(unknown)",
                            l.lineNumber);
            }
            if (chain->extraLayers > 0) {
                out.Appendf("    ... %u more layers dropped (cap = %zu)\n",
                            chain->extraLayers, redscope::snap::kMaxWrapLayers);
            }
        }
        if (matched == 0) {
            out.Append("  (no scripted frames match any discovered chain)\n");
        } else if (matched < chainCount) {
            out.Appendf("\n(%u additional chains discovered but not in the scripted stack; "
                        "not shown here)\n", chainCount - matched);
        }
    } else {
        out.Appendf("(no scripted frames in flight; cannot correlate. %u chains known.)\n",
                    chainCount);
    }

    const CrashClass crashClass = CurrentCrashClass();
    const bool scriptedWithMatches =
        (crashClass == CrashClass::Scripted) && (matched > 0);
    if (scriptedWithMatches) {
        out.Append("\n(per-mod ranking suppressed; scripted stack attribution above "
                   "is the direct signal.)\n\n");
        return;
    }

    struct ModCount { const char* name; uint32_t chains; };
    constexpr size_t kMaxModsTracked = 64;
    ModCount mods[kMaxModsTracked] = {};
    uint32_t modCount = 0;

    for (const auto& c : tbl.chains) {
        const char* seen[redscope::snap::kMaxWrapLayers] = {};
        uint32_t seenCount = 0;
        for (uint32_t k = 0; k < c.layerCount; ++k) {
            const char* name = c.layers[k].modName;
            if (!name || !name[0]) continue;
            bool dup = false;
            for (uint32_t s2 = 0; s2 < seenCount; ++s2) {
                if (std::strcmp(seen[s2], name) == 0) { dup = true; break; }
            }
            if (dup) continue;
            if (seenCount < redscope::snap::kMaxWrapLayers) seen[seenCount++] = name;

            bool found = false;
            for (uint32_t m = 0; m < modCount; ++m) {
                if (std::strcmp(mods[m].name, name) == 0) {
                    ++mods[m].chains;
                    found = true;
                    break;
                }
            }
            if (!found && modCount < kMaxModsTracked) {
                mods[modCount++] = { name, 1u };
            }
        }
    }

    constexpr size_t kMaxEmitted = 5;
    const uint32_t emitCount = modCount < kMaxEmitted ? modCount : (uint32_t)kMaxEmitted;
    for (uint32_t i = 0; i < emitCount; ++i) {
        uint32_t maxIdx = i;
        for (uint32_t j = i + 1; j < modCount; ++j) {
            if (mods[j].chains > mods[maxIdx].chains) maxIdx = j;
        }
        if (maxIdx != i) std::swap(mods[i], mods[maxIdx]);
    }

    if (modCount > 0) {
        const char* headerLabel =
            (crashClass == CrashClass::Native)
                ? "Top %u mods by wrap-chain count (ambient context; crash is native):\n"
                : "Top %u mods by wrap-chain count (unique methods wrapped):\n";
        out.Appendf(headerLabel, emitCount);
        for (uint32_t i = 0; i < emitCount; ++i) {
            out.Appendf("  %-40s %u chains\n", mods[i].name, mods[i].chains);
        }
        if (modCount > emitCount) {
            out.Appendf("  ... (%u more mods not shown)\n", modCount - emitCount);
        }
    }
    out.Append("\n");
}

void EmitSinceLastLaunch(PreallocatedBuffer& out) {
    out.Append("--- Modlist changes since last launch -----------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) { out.Append("(snapshot not yet available)\n\n"); return; }

    const auto& d = s->modDiff;
    if (!d.priorFilePresent) {
        out.Append("(first run since REDscope install; no prior launch recorded)\n\n");
        return;
    }
    if (d.priorTimestampUnix > 0) {
        std::tm tmv{};
        time_t t = (time_t)d.priorTimestampUnix;
        localtime_s(&tmv, &t);
        char tsBuf[64];
        std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", &tmv);
        out.Appendf("Last launch: %s  (%u mods tracked)\n", tsBuf, d.priorModCount);
    } else {
        out.Appendf("Last launch: (unknown time)  (%u mods tracked)\n", d.priorModCount);
    }

    if (d.added.empty() && d.removed.empty() && d.updated.empty()) {
        out.Append("No modlist changes since last launch.\n\n");
        return;
    }

    if (!d.added.empty()) {
        out.Appendf("  +%zu added:\n", d.added.size());
        for (const auto& m : d.added) {
            out.Appendf("    %s%s%s\n",
                        m.name.c_str(),
                        m.version.empty() ? "" : " ",
                        m.version.c_str());
        }
    }
    if (!d.removed.empty()) {
        out.Appendf("  -%zu removed:\n", d.removed.size());
        for (const auto& m : d.removed) {
            out.Appendf("    %s%s%s\n",
                        m.name.c_str(),
                        m.version.empty() ? "" : " ",
                        m.version.c_str());
        }
    }
    if (!d.updated.empty()) {
        out.Appendf("  ~%zu updated:\n", d.updated.size());
        for (const auto& [from, to] : d.updated) {
            out.Appendf("    %s: %s -> %s\n",
                        from.name.c_str(),
                        from.version.empty() ? "(unversioned)" : from.version.c_str(),
                        to.version.empty()   ? "(unversioned)" : to.version.c_str());
        }
    }
    out.Append("\n");
}

void EmitLoadedModules(PreallocatedBuffer& out) {
    out.Append("--- Loaded modules ------------------------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) {
        out.Append("(snapshot not yet available)\n\n");
        return;
    }

    EmitGroup(out, *s, ModuleKind::Mod,  "Mods");
    EmitGroup(out, *s, ModuleKind::Game, "Game");

    uint32_t systemCount  = CountKind(*s, ModuleKind::System);
    uint32_t unknownCount = CountKind(*s, ModuleKind::Unknown);

    if (unknownCount > 0) {
        EmitGroup(out, *s, ModuleKind::Unknown, "Other");
    }

    out.Appendf("System DLLs: %u (Windows runtime, omitted from report)\n", systemCount);

    if (s->moduleOverflow > 0) {
        out.Appendf("\n... (%u more modules omitted; increase kMaxModules)\n",
                    (unsigned)s->moduleOverflow);
    }
    out.Append("\n");
}

namespace {

struct EmitArgs_Buf       { PreallocatedBuffer* buf; };
struct EmitArgs_BufEp     { PreallocatedBuffer* buf; const EXCEPTION_POINTERS* ep; };
struct EmitArgs_BufEpTid  { PreallocatedBuffer* buf; const EXCEPTION_POINTERS* ep; DWORD tid; };
struct EmitArgs_BufEpCfg  { PreallocatedBuffer* buf; const EXCEPTION_POINTERS* ep; uint32_t slotCount; };
struct EmitArgs_BufCrash  { PreallocatedBuffer* buf; std::chrono::system_clock::time_point crashTime; };

using EmitThunk = void(*)(const void*);

void Thunk_EmitHeader          (const void* p) { auto* a = (const EmitArgs_BufEpTid*)p; EmitHeader(*a->buf, a->ep, a->tid); }
void Thunk_EmitBreadcrumbs     (const void* p) { EmitBreadcrumbs(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitSystem          (const void* p) { EmitSystem(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitNativeCallstack (const void* p) { auto* a = (const EmitArgs_BufEp*)p; EmitNativeCallstack(*a->buf, a->ep); }
void Thunk_EmitScriptCallstack (const void* p) { EmitScriptCallstack(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitLastActive       (const void* p) { auto* a = (const EmitArgs_BufEp*)p; EmitLastActive(*a->buf, a->ep); }
void Thunk_EmitSetupIntegrity   (const void* p) { EmitSetupIntegritySection(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitArchiveConflicts (const void* p) { EmitArchiveConflictsSection(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitRegisters       (const void* p) { auto* a = (const EmitArgs_BufEp*)p; EmitRegisters(*a->buf, a->ep); }
void Thunk_EmitStack           (const void* p) { auto* a = (const EmitArgs_BufEpCfg*)p; EmitStack(*a->buf, a->ep, a->slotCount); }
void Thunk_EmitProcessMemory   (const void* p) { EmitProcessMemory(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitRecentLogs      (const void* p) { auto* a = (const EmitArgs_BufCrash*)p; EmitRecentLogs(*a->buf, a->crashTime); }
void Thunk_EmitSinceLastLaunch (const void* p) { EmitSinceLastLaunch(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitLoadedModules   (const void* p) { EmitLoadedModules(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitInstalledMods   (const void* p) { EmitInstalledMods(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitWrapChains      (const void* p) { EmitWrapChains(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitEngineState     (const void* p) { EmitEngineState(*((const EmitArgs_Buf*)p)->buf); }
void Thunk_EmitResourceLoader  (const void* p) { EmitResourceLoaderSection(*((const EmitArgs_Buf*)p)->buf); }

void LogStage(const char* label) {
    redscope::log::Warn(std::string("emit: ") + label);
    redscope::log::Flush();
}
void LogStageFault(const char* label, DWORD code) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "emit FAULTED: %s code=0x%08lX", label, (unsigned long)code);
    redscope::log::Warn(std::string_view(msg));
    redscope::log::Flush();
}
void EmitFaultBanner(PreallocatedBuffer& out, const char* label, DWORD code) {
    out.Appendf("\n[!! SECTION FAULTED: %s code=0x%08lX -- contents may be incomplete]\n\n",
                label, (unsigned long)code);
}

bool RunEmitSafe(PreallocatedBuffer& buf, const char* label,
                 EmitThunk thunk, const void* arg) {
    LogStage(label);
    DWORD faultCode = 0;
    __try {
        thunk(arg);
        return true;
    } __except ((faultCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
        LogStageFault(label, faultCode);
        EmitFaultBanner(buf, label, faultCode);
        return false;
    }
}

void CheckpointFile(PreallocatedBuffer& buf, const wchar_t* outPath) {
    __try {
        (void)buf.WriteToFile(outPath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteJsonSidecarFile(const wchar_t* reportPath, const EXCEPTION_POINTERS* ep,
                          std::chrono::system_clock::time_point crashTime) {
    __try {
        size_t plen = 0;
        while (reportPath[plen]) { ++plen; }
        if (plen + 6 > 1024) { return; }
        wchar_t jsonPath[1024];
        size_t n = 0;
        while (reportPath[n] && n < 1018) { jsonPath[n] = reportPath[n]; ++n; }
        const wchar_t kExt[] = L".json";
        for (size_t i = 0; i < 5 && n < 1023; ++i) { jsonPath[n++] = kExt[i]; }
        jsonPath[n] = L'\0';
        auto& sb = redscope::SidecarBuffer();
        sb.Reset();
        redscope::report::BuildJsonSidecar(sb, ep, redscope::snap::Current(), crashTime);
        WriteBytesToFile(jsonPath, sb.Data(), sb.Size());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}

bool WriteMinimalReport(const wchar_t* outPath, const EXCEPTION_POINTERS* ep, DWORD threadId,
                        std::chrono::system_clock::time_point crashTime) {
    auto& buf = MainCrashBuffer();
    buf.Reset();

    SetReportOutPath(outPath);

    __try {
        FrameView frames[16] = {};
        size_t    frameCount = 0;
        char      modBuf[16][kModuleNameCap] = {};

        if (ep && ep->ContextRecord) {
            CONTEXT ctx = *ep->ContextRecord;
            STACKFRAME64 frame{};
            frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;

            HANDLE proc = ::GetCurrentProcess();
            HANDLE thrd = ::GetCurrentThread();
            for (size_t i = 0; i < 16; ++i) {
                if (i == 0) {
                    uintptr_t pc = ctx.Rip;
                    HMODULE mh = nullptr;
                    ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCSTR>(pc), &mh);
                    if (mh) {
                        char path[MAX_PATH] = {};
                        ::GetModuleFileNameA(mh, path, MAX_PATH);
                        const char* name = std::strrchr(path, '\\');
                        name = name ? name + 1 : path;
                        std::strncpy(modBuf[frameCount], name, kModuleNameCap - 1);
                        frames[frameCount].moduleName = modBuf[frameCount];
                        frames[frameCount].symbolName = nullptr;
                        ++frameCount;
                    }
                }
                if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thrd, &frame, &ctx,
                                   nullptr, ::SymFunctionTableAccess64,
                                   ::SymGetModuleBase64, nullptr)) break;
                if (frame.AddrPC.Offset == 0) break;
                HMODULE mh = nullptr;
                ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(frame.AddrPC.Offset), &mh);
                if (!mh) continue;
                if (frameCount >= 16) break;
                char path[MAX_PATH] = {};
                ::GetModuleFileNameA(mh, path, MAX_PATH);
                const char* name = std::strrchr(path, '\\');
                name = name ? name + 1 : path;
                std::strncpy(modBuf[frameCount], name, kModuleNameCap - 1);
                frames[frameCount].moduleName = modBuf[frameCount];
                frames[frameCount].symbolName = nullptr;
                ++frameCount;
            }
        }

        const uint32_t scriptDepth = redscope::scriptstack::Current().depth;
        SetCurrentCrashClass(ClassifyCrash(frames, frameCount, scriptDepth));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const uint32_t scriptDepth = redscope::scriptstack::Current().depth;
        SetCurrentCrashClass(scriptDepth > 0 ? CrashClass::Scripted : CrashClass::Native);
    }

    const EmitArgs_BufEpTid  aHeader    = { &buf, ep, threadId };
    const EmitArgs_Buf       aBuf       = { &buf };
    const EmitArgs_BufEp     aEp        = { &buf, ep };
    const EmitArgs_BufEpCfg  aStack     = { &buf, ep, redscope::GetConfig().stackDumpSlotCount };
    const EmitArgs_BufCrash  aLogs      = { &buf, crashTime };

    RunEmitSafe(buf, "header",                &Thunk_EmitHeader,          &aHeader);
    RunEmitSafe(buf, "last active",           &Thunk_EmitLastActive,      &aEp);
    RunEmitSafe(buf, "setup integrity",       &Thunk_EmitSetupIntegrity,  &aBuf);
    RunEmitSafe(buf, "archive conflicts",     &Thunk_EmitArchiveConflicts,&aBuf);
    RunEmitSafe(buf, "breadcrumbs",           &Thunk_EmitBreadcrumbs,     &aBuf);
    RunEmitSafe(buf, "system",                &Thunk_EmitSystem,          &aBuf);
    RunEmitSafe(buf, "native stack",          &Thunk_EmitNativeCallstack, &aEp);
    RunEmitSafe(buf, "script stack",          &Thunk_EmitScriptCallstack, &aBuf);
    RunEmitSafe(buf, "registers",             &Thunk_EmitRegisters,       &aEp);
    CheckpointFile(buf, outPath);
    WriteJsonSidecarFile(outPath, ep, crashTime);
    RunEmitSafe(buf, "stack memory",          &Thunk_EmitStack,           &aStack);
    CheckpointFile(buf, outPath);
    RunEmitSafe(buf, "process memory",        &Thunk_EmitProcessMemory,   &aBuf);
    RunEmitSafe(buf, "engine state",          &Thunk_EmitEngineState,     &aBuf);
    RunEmitSafe(buf, "resources loading",     &Thunk_EmitResourceLoader,  &aBuf);
    RunEmitSafe(buf, "recent logs",           &Thunk_EmitRecentLogs,      &aLogs);
    CheckpointFile(buf, outPath);
    RunEmitSafe(buf, "since last launch",     &Thunk_EmitSinceLastLaunch, &aBuf);
    RunEmitSafe(buf, "loaded modules",        &Thunk_EmitLoadedModules,   &aBuf);
    CheckpointFile(buf, outPath);
    RunEmitSafe(buf, "installed mods",        &Thunk_EmitInstalledMods,   &aBuf);
    RunEmitSafe(buf, "wrap chains",           &Thunk_EmitWrapChains,      &aBuf);

    char sizeTmp[96];
    std::snprintf(sizeTmp, sizeof(sizeTmp),
                  "emit: done, buffer %zu / %zu bytes, writing to file",
                  buf.Size(), buf.Capacity());
    redscope::log::Warn(std::string_view(sizeTmp));
    redscope::log::Flush();

    bool reportOk = buf.WriteToFile(outPath);
    if (reportOk) {
        redscope::log::Warn("emit: WriteToFile OK");
        redscope::log::Flush();
    } else {
        redscope::log::Warn("emit: WriteToFile FAILED, falling back to literal last-resort write");
        redscope::log::Flush();
        static const char kMsg[] =
            "REDscope FAILED (main WriteFile failed — check crashes dir ACL / disk / filter drivers)\n";
        WriteBytesToFile(outPath, kMsg, sizeof(kMsg) - 1);
    }

    return reportOk;
}

bool WriteStackOverflowReport(const wchar_t* outPath, const EXCEPTION_POINTERS* ep, DWORD threadId,
                              std::chrono::system_clock::time_point crashTime) {
    (void)crashTime;
    auto& buf = MainCrashBuffer();
    buf.Reset();
    EmitHeader(buf, ep, threadId);
    buf.Append("NOTE: EXCEPTION_STACK_OVERFLOW detected; emitting reduced report.\n");
    buf.Append("      No stack walk, no module list, no pointer classification.\n\n");

    EmitScriptCallstack(buf);

    if (ep && ep->ContextRecord) {
        const CONTEXT& c = *ep->ContextRecord;
        buf.Append("--- Registers -----------------------------------------------------------------\n");
        buf.Appendf("RIP 0x%016llX  RSP 0x%016llX  RBP 0x%016llX\n",
                    (unsigned long long)c.Rip, (unsigned long long)c.Rsp, (unsigned long long)c.Rbp);
        buf.Appendf("RAX 0x%016llX  RBX 0x%016llX  RCX 0x%016llX  RDX 0x%016llX\n",
                    (unsigned long long)c.Rax, (unsigned long long)c.Rbx,
                    (unsigned long long)c.Rcx, (unsigned long long)c.Rdx);
        buf.Appendf("RSI 0x%016llX  RDI 0x%016llX  R8  0x%016llX  R9  0x%016llX\n",
                    (unsigned long long)c.Rsi, (unsigned long long)c.Rdi,
                    (unsigned long long)c.R8,  (unsigned long long)c.R9);
        buf.Appendf("R10 0x%016llX  R11 0x%016llX  R12 0x%016llX  R13 0x%016llX\n",
                    (unsigned long long)c.R10, (unsigned long long)c.R11,
                    (unsigned long long)c.R12, (unsigned long long)c.R13);
        buf.Appendf("R14 0x%016llX  R15 0x%016llX\n\n",
                    (unsigned long long)c.R14, (unsigned long long)c.R15);
    }

    const Snapshot* snap = redscope::snap::Current();
    if (snap) {
        const auto& e = snap->engineLive;
        if (e.hasData) {
            buf.Append("--- Engine state (from last published snapshot) -------------------------------\n");
            if (e.engineReadOk) {
                buf.Appendf("Engine state: %s  scriptsLoaded=%s  closing=%s  isEP1=%s\n",
                            redscope::snap::EngineStateName(e.engineStateRaw),
                            e.scriptsLoaded ? "true" : "false",
                            e.isClosing ? "true" : "false",
                            e.isEP1 ? "true" : "false");
            }
            if (e.gpuReadOk) {
                buf.Appendf("GPU memory: %u / %u MB used (budget %u MB)\n",
                            e.gpuUsedMB, e.gpuDedicatedMB, e.gpuBudgetMB);
            }
            buf.Appendf("OOM suspected: %s%s%s\n",
                        e.oomSuspected ? "true" : "false",
                        e.oomBasis[0] ? "  basis=" : "",
                        e.oomBasis[0] ? e.oomBasis : "");
            buf.Append("\n");
        }

        const auto& g = snap->gameStateLive;
        if (g.hasData && g.count > 0) {
            buf.Append("--- Live game state (last published) ------------------------------------------\n");
            const uint32_t cap = g.count > 16u ? 16u : g.count;
            for (uint32_t i = 0; i < cap; ++i) {
                const auto& kv = g.entries[i];
                if (kv.key[0]) buf.Appendf("  %-20s  %s\n", kv.key, kv.value);
            }
            buf.Append("\n");
        }

        const auto& d = snap->modDiff;
        if (d.priorFilePresent && d.parsedOk) {
            const bool any = !(d.added.empty() && d.removed.empty() && d.updated.empty());
            if (any) {
                buf.Append("--- Mods changed since last launch --------------------------------------------\n");
                const size_t maxRows = 8;
                const size_t na = d.added.size() > maxRows ? maxRows : d.added.size();
                for (size_t i = 0; i < na; ++i) {
                    buf.Appendf("  + %s %s\n", d.added[i].name.c_str(), d.added[i].version.c_str());
                }
                const size_t nu = d.updated.size() > maxRows ? maxRows : d.updated.size();
                for (size_t i = 0; i < nu; ++i) {
                    buf.Appendf("  ~ %s: %s -> %s\n",
                                d.updated[i].first.name.c_str(),
                                d.updated[i].first.version.c_str(),
                                d.updated[i].second.version.c_str());
                }
                const size_t nr = d.removed.size() > maxRows ? maxRows : d.removed.size();
                for (size_t i = 0; i < nr; ++i) {
                    buf.Appendf("  - %s\n", d.removed[i].name.c_str());
                }
                buf.Append("\n");
            }
        }
    }

    if (buf.WriteToFile(outPath)) return true;

    static const char kMsg[] = "REDscope FAILED (stack overflow + WriteFile failed)\n";
    WriteBytesToFile(outPath, kMsg, sizeof(kMsg) - 1);
    return false;
}

}
