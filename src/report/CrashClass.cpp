#include "CrashClass.h"
#include "../util/StringUtils.h"

namespace redscope::report {

namespace {
CrashClass g_current = CrashClass::Unknown;
}

CrashClass CurrentCrashClass() noexcept { return g_current; }
void SetCurrentCrashClass(CrashClass c) noexcept { g_current = c; }

bool IsRed4extModule(const char* moduleName) noexcept {
    return IEqualsAscii(moduleName, "RED4ext.dll");
}

bool IsScriptRuntimeSymbol(const char* symbolName) noexcept {
    if (!symbolName || !*symbolName) return false;

    static const char* const kPatterns[] = {
        "CClass::Execute",
        "CBaseFunction::Execute",
        "CScriptedFunction",
        "CStackFrame",
        "CallScriptFunction",
        "ScriptFunction::Execute",
    };
    for (const char* pat : kPatterns) {
        if (IContainsAscii(symbolName, pat)) return true;
    }
    return false;
}

CrashClass ClassifyCrash(const FrameView* frames,
                         size_t count,
                         uint32_t scriptStackDepth) noexcept {
    if (scriptStackDepth > 0) return CrashClass::Scripted;

    if (!frames || count == 0) return CrashClass::Unknown;

    for (size_t i = 0; i < count; ++i) {
        if (IsRed4extModule(frames[i].moduleName)) return CrashClass::Scripted;
        if (IsScriptRuntimeSymbol(frames[i].symbolName)) return CrashClass::Scripted;
    }
    return CrashClass::Native;
}

const char* CrashClassLabel(CrashClass c) noexcept {
    switch (c) {
        case CrashClass::Native:   return "native";
        case CrashClass::Scripted: return "scripted";
        case CrashClass::Unknown:
        default:                   return "unknown";
    }
}

}
