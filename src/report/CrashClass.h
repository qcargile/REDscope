#pragma once
#include <cstddef>
#include <cstdint>

namespace redscope::report {

enum class CrashClass : uint8_t {
    Unknown  = 0,
    Native   = 1,
    Scripted = 2,
};

struct FrameView {
    const char* moduleName = nullptr;
    const char* symbolName = nullptr;
};

bool IsRed4extModule(const char* moduleName) noexcept;

bool IsScriptRuntimeSymbol(const char* symbolName) noexcept;

CrashClass ClassifyCrash(const FrameView* frames,
                         size_t count,
                         uint32_t scriptStackDepth) noexcept;

const char* CrashClassLabel(CrashClass c) noexcept;

CrashClass CurrentCrashClass() noexcept;
void       SetCurrentCrashClass(CrashClass c) noexcept;

}
