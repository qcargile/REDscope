#include "NestedFallback.h"
#include "../util/PreallocatedBuffer.h"
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <string>
#include <string_view>

namespace redscope::handler {

static void WriteLiteralFallback(const wchar_t* path) {
    static const char kMsg[] = "REDscope FAILED (fallback buffer alloc failed)\n";
    redscope::WriteBytesToFile(path, kMsg, sizeof(kMsg) - 1);
}

static void WriteFailedReport(const std::filesystem::path& crashesDir,
                              EXCEPTION_POINTERS* outerEp,
                              DWORD threadId,
                              const EXCEPTION_RECORD* innerEr) {
    using clock = std::chrono::system_clock;
    auto now = clock::to_time_t(clock::now());
    std::tm tmv{}; localtime_s(&tmv, &now);
    wchar_t name[80];
    std::swprintf(name, 80, L"REDscope-FAILED-%04d-%02d-%02d_%02d-%02d-%02d.crash",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    auto path = (crashesDir / name).wstring();

    PreallocatedBuffer b;
    b.Reserve(8192);
    if (b.Capacity() == 0) {
        WriteLiteralFallback(path.c_str());
        return;
    }

    b.Append("REDscope FAILED while generating report.\n");
    b.Appendf("Thread: %lu\n", threadId);
    if (outerEp && outerEp->ExceptionRecord) {
        b.Appendf("Outer exception: 0x%08lX at 0x%p\n",
                  outerEp->ExceptionRecord->ExceptionCode,
                  outerEp->ExceptionRecord->ExceptionAddress);
    }
    if (innerEr) {
        b.Appendf("Inner exception: 0x%08lX at 0x%p\n",
                  innerEr->ExceptionCode, innerEr->ExceptionAddress);
    }
    auto& main = MainCrashBuffer();
    if (main.Size()) {
        b.Append("Partial main-buffer contents follow:\n---\n");
        size_t take = main.Size() > 4096 ? 4096 : main.Size();
        b.Append(std::string_view(main.Data(), take));
    }
    b.WriteToFile(path.c_str());
}

bool RunWithNestedFallback(EXCEPTION_POINTERS* outerEp, DWORD threadId,
                           const std::filesystem::path& crashesDir,
                           std::chrono::system_clock::time_point crashTime,
                           void (*body)(EXCEPTION_POINTERS*, DWORD, const std::filesystem::path&,
                                        std::chrono::system_clock::time_point)) {
    EXCEPTION_RECORD inner{};
    bool ok = false;
    __try {
        body(outerEp, threadId, crashesDir, crashTime);
        ok = true;
    } __except ((inner = *GetExceptionInformation()->ExceptionRecord, EXCEPTION_EXECUTE_HANDLER)) {
        WriteFailedReport(crashesDir, outerEp, threadId, &inner);
        ok = false;
    }
    return ok;
}

}
