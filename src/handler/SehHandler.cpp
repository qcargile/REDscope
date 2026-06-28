#include "SehHandler.h"
#include "NestedFallback.h"
#include "../report/Sections.h"
#include "../snapshot/SnapshotWorker.h"
#include "../hooks/DispatchHook.h"
#include "../Logger.h"
#include <windows.h>
#include <dbghelp.h>
#include <chrono>
#include <ctime>
#include <atomic>
#include <cstdio>
#include <string>

namespace redscope::handler {
namespace {

std::filesystem::path g_crashesDir;
PVOID                 g_veh = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevUef = nullptr;
std::atomic<int>      g_handlerInProgress{0};
bool                  g_respectDebugger = true;
bool                  g_terminateAfterReport = false;

bool ShouldHandle(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_STACK_CHECK:
        case 0xC0000374:
        case 0xC0000409:
        case 0xC0000420:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_INVALID_DISPOSITION:
        case 0x887A0005:
        case 0x887A0006:
        case 0x887A0007:
            return true;
        default:
            return false;
    }
}

std::wstring BuildCrashPathIn(const std::filesystem::path& dir) {
    using clock = std::chrono::system_clock;
    auto now = clock::to_time_t(clock::now());
    std::tm tmv{}; localtime_s(&tmv, &now);
    wchar_t name[64];
    std::swprintf(name, 64, L"REDscope-%04d-%02d-%02d_%02d-%02d-%02d.crash",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return (dir / name).wstring();
}

void BodyWriteReport(EXCEPTION_POINTERS* ep, DWORD tid, const std::filesystem::path& dir,
                     std::chrono::system_clock::time_point crashTime) {
    auto path = BuildCrashPathIn(dir);
    redscope::report::WriteMinimalReport(path.c_str(), ep, tid, crashTime);
}

LONG WINAPI Handler(EXCEPTION_POINTERS* ep) {
    if (g_respectDebugger && ::IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (!ShouldHandle(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

    int expected = 0;
    if (!g_handlerInProgress.compare_exchange_strong(expected, 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    redscope::snap::FreezeForCrash();

    redscope::hooks::dispatch::ResolveStackNames();
    redscope::hooks::dispatch::ResolveHeartbeatNames();

    auto crashTime = std::chrono::system_clock::now();

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD tid  = ::GetCurrentThreadId();

    char tmp[128];
    std::snprintf(tmp, sizeof(tmp), "VEH caught exception code 0x%08lX on thread %lu",
                  (unsigned long)code, (unsigned long)tid);
    redscope::log::Warn(tmp);
    redscope::log::Flush();

    if (code == EXCEPTION_STACK_OVERFLOW) {
        auto path = BuildCrashPathIn(g_crashesDir);
        redscope::log::Warn("VEH: writing stack-overflow report");
        redscope::log::Flush();
        bool ok = redscope::report::WriteStackOverflowReport(path.c_str(), ep, tid, crashTime);
        std::snprintf(tmp, sizeof(tmp),
                      "VEH: stack-overflow write %s", ok ? "OK" : "FAILED");
        redscope::log::Warn(tmp);
        redscope::log::Flush();
        g_handlerInProgress.fetch_sub(1);
        ::TerminateProcess(::GetCurrentProcess(), code);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    redscope::log::Warn("VEH: entering report body via NestedFallback");
    redscope::log::Flush();
    bool ok = RunWithNestedFallback(ep, tid, g_crashesDir, crashTime, &BodyWriteReport);
    g_handlerInProgress.fetch_sub(1);
    std::snprintf(tmp, sizeof(tmp),
                  "VEH: report body %s (nested fallback %s)",
                  ok ? "completed" : "faulted",
                  ok ? "unused"    : "wrote FAILED-*.crash");
    redscope::log::Warn(tmp);
    redscope::log::Flush();

    if (g_terminateAfterReport) {
        ::TerminateProcess(::GetCurrentProcess(), code);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK VehTrampoline(EXCEPTION_POINTERS* ep) { return Handler(ep); }
LONG WINAPI UefTrampoline(EXCEPTION_POINTERS* ep) {
    return g_prevUef ? g_prevUef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}

void Arm(const std::filesystem::path& crashesDir) {
    g_crashesDir = crashesDir;
    std::error_code ec;
    std::filesystem::create_directories(g_crashesDir, ec);
    g_veh = ::AddVectoredExceptionHandler(1, &VehTrampoline);
    g_prevUef = ::SetUnhandledExceptionFilter(&UefTrampoline);
    log::Info("SEH handler armed; crashes dir: " + g_crashesDir.string());
}

void Configure(bool respectDebugger, bool terminateAfterReport) {
    g_respectDebugger = respectDebugger;
    g_terminateAfterReport = terminateAfterReport;

    ULONG stackReserve = 64 * 1024;
    if (!::SetThreadStackGuarantee(&stackReserve)) {
        log::Warn("SetThreadStackGuarantee failed; stack-overflow handling may be unreliable.");
    }

    auto probePath = (g_crashesDir / L".redscope_writable.probe").wstring();
    HANDLE hProbe = ::CreateFileW(probePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (hProbe == INVALID_HANDLE_VALUE) {
        log::Warn("crashes dir not writable at init: " + g_crashesDir.string()
                  + " (GetLastError=" + std::to_string(::GetLastError()) + ")");
    } else {
        ::CloseHandle(hProbe);
    }

    HANDLE proc = ::GetCurrentProcess();
    ::SymInitialize(proc, nullptr, TRUE);

    auto symbolsDir = g_crashesDir.parent_path() / L"symbols";
    std::error_code symEc;
    std::filesystem::create_directories(symbolsDir, symEc);
    std::string searchPath = symbolsDir.string();
    char existingPath[2048] = {};
    if (::SymGetSearchPath(proc, existingPath, (DWORD)sizeof(existingPath)) && existingPath[0] != '\0') {
        searchPath += ";";
        searchPath += existingPath;
    }
    ::SymSetSearchPath(proc, searchPath.c_str());
    log::Info("Symbol search path: " + searchPath);
}

void Uninstall() {
    if (g_veh) { ::RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    if (g_prevUef) { ::SetUnhandledExceptionFilter(g_prevUef); g_prevUef = nullptr; }
}

}
