#include "Sections.h"
#include "../util/PreallocatedBuffer.h"
#include <windows.h>

namespace redscope::report {

namespace {
const wchar_t* g_reportOutPath = nullptr;
}

void SetReportOutPath(const wchar_t* outPath) noexcept {
    g_reportOutPath = outPath;
}

const wchar_t* GetReportOutPath() noexcept {
    return g_reportOutPath;
}

void CheckpointBuffer(PreallocatedBuffer& buf, const wchar_t* outPath) noexcept {
    if (!outPath) return;
    __try {
        (void)buf.WriteToFile(outPath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}
