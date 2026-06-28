#pragma once
#include <windows.h>
#include <chrono>
#include <cstdint>
#include "../util/PreallocatedBuffer.h"

namespace redscope { struct Snapshot; }

namespace redscope::report {

void EmitHeader(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep, DWORD threadId);
void EmitBreadcrumbs(PreallocatedBuffer& out);
void EmitSystem(PreallocatedBuffer& out);
void EmitNativeCallstack(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep);

struct ModStackFrame {
    int          frameIndex = 0;
    uintptr_t    pc         = 0;
    const char*  moduleName = nullptr;
    uintptr_t    moduleBase = 0;
};
size_t GatherModFramesOnStack(const EXCEPTION_POINTERS* ep, const Snapshot* snap,
                              ModStackFrame* out, size_t outCap);
void EmitScriptCallstack(PreallocatedBuffer& out);

void EmitLastActive(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep);
void EmitRegisters(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep);
void EmitStack(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep,
               uint32_t slotCountOverride = 0);
void EmitProcessMemory(PreallocatedBuffer& out);
void EmitEngineState(PreallocatedBuffer& out);
void EmitRecentLogs(PreallocatedBuffer& out, std::chrono::system_clock::time_point crashTime);
void EmitLoadedModules(PreallocatedBuffer& out);
void EmitSinceLastLaunch(PreallocatedBuffer& out);

void EmitInstalledMods(PreallocatedBuffer& out);
void EmitWrapChains(PreallocatedBuffer& out);

void EmitSetupIntegritySection(PreallocatedBuffer& out);
void EmitArchiveConflictsSection(PreallocatedBuffer& out);
void EmitResourceLoaderSection(PreallocatedBuffer& out);

bool WriteMinimalReport(const wchar_t* outPath, const EXCEPTION_POINTERS* ep, DWORD threadId,
                        std::chrono::system_clock::time_point crashTime);

void           SetReportOutPath(const wchar_t* outPath) noexcept;
const wchar_t* GetReportOutPath() noexcept;

void CheckpointBuffer(PreallocatedBuffer& buf, const wchar_t* outPath) noexcept;

bool WriteStackOverflowReport(const wchar_t* outPath, const EXCEPTION_POINTERS* ep, DWORD threadId,
                              std::chrono::system_clock::time_point crashTime);

}
