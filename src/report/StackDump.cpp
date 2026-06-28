#include "Sections.h"
#include "../rtti/PointerType.h"
#include "../rtti/HandleDetect.h"
#include "../snapshot/RttiSnapshot.h"
#include "../snapshot/Snapshot.h"
#include "../snapshot/SnapshotWorker.h"
#include "../util/SehGuardedRead.h"
#include "../util/StringUtils.h"
#include <cstdint>
#include <cstdio>
#include <dbghelp.h>

namespace redscope::report {

static bool IsFrameworkSelfModule(const char* name) {
    if (!name) return false;
    static const char* const kFramework[] = {
        "redscope.dll", "red4ext.dll", "cyber_engine_tweaks.asi",
        "scc.dll", "scc_lib.dll", "redsocket.dll",
    };
    for (const char* f : kFramework) {
        if (IEqualsAscii(name, f)) return true;
    }
    return false;
}

size_t GatherModFramesOnStack(const EXCEPTION_POINTERS* ep,
                              const Snapshot* snap,
                              ModStackFrame* out,
                              size_t outCap) {
    if (!ep || !ep->ContextRecord || !snap || !out || outCap == 0) return 0;

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset    = ctx.Rip; f.AddrPC.Mode    = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;

    HANDLE proc = ::GetCurrentProcess();
    HANDLE thrd = ::GetCurrentThread();

    size_t written = 0;
    auto recordIfMod = [&](int idx, uintptr_t pc) {
        if (written >= outCap) return;
        auto* mf = redscope::FindModuleByPC(*snap, pc);
        if (!mf) return;
        if (mf->kind != ModuleKind::Mod) return;
        const char* nm = snap->moduleNames[mf->nameIndex];
        if (IsFrameworkSelfModule(nm)) return;
        out[written++] = { idx, pc, nm, mf->base };
    };

    recordIfMod(0, ctx.Rip);

    int emitted = 1;
    for (int i = 0; emitted < 48; ++i) {
        if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thrd, &f, &ctx,
                           nullptr, ::SymFunctionTableAccess64,
                           ::SymGetModuleBase64, nullptr)) break;
        if (f.AddrPC.Offset == 0) break;
        if (i == 0 && f.AddrPC.Offset == ctx.Rip) continue;
        recordIfMod(emitted, f.AddrPC.Offset);
        ++emitted;
    }
    return written;
}

static constexpr uint32_t kDefaultStackDumpSlotCount = 512;

static inline void EmitSlotPrefix(PreallocatedBuffer& out, uint32_t byteOffset, uint64_t qword) {
    if (byteOffset <= 0xFFFF) {
        out.Appendf("[SP+0x%04X]  0x%016llX", byteOffset, (unsigned long long)qword);
    } else {
        out.Appendf("[SP+0x%05X]  0x%016llX", byteOffset, (unsigned long long)qword);
    }
}

static inline void EmitTruncation(PreallocatedBuffer& out, uint32_t byteOffset) {
    if (byteOffset <= 0xFFFF) {
        out.Appendf("(stack dump truncated at SP+0x%04X - read fault; end of stack mapping)\n",
                    byteOffset);
    } else {
        out.Appendf("(stack dump truncated at SP+0x%05X - read fault; end of stack mapping)\n",
                    byteOffset);
    }
}

static bool ProbeHandleRefBlock(uintptr_t slotAddrRef,
                                uint32_t& outStrong,
                                uint32_t& outWeak) noexcept {
    uint64_t refPtr = 0;
    if (!redscope::SehSafeReadValue<uint64_t>(refPtr,
            reinterpret_cast<const void*>(slotAddrRef))) return false;
    if (refPtr == 0) return false;

    struct { uint32_t strong; uint32_t weak; } counts{};
    if (!redscope::SehSafeReadValue(counts,
            reinterpret_cast<const void*>(refPtr))) return false;

    if (!redscope::rtti::LooksLikeRefBlock(counts.strong, counts.weak)) return false;

    outStrong = counts.strong;
    outWeak   = counts.weak;
    return true;
}

void EmitStack(PreallocatedBuffer& out, const EXCEPTION_POINTERS* ep,
               uint32_t slotCountOverride) {
    out.Append("--- Stack memory --------------------------------------------------------------\n");
    if (!ep || !ep->ContextRecord) { out.Append("(no exception context)\n\n"); return; }

    const uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
    const uint32_t slotCount = slotCountOverride ? slotCountOverride : kDefaultStackDumpSlotCount;

    constexpr uint32_t kCheckpointEverySlots = 64;
    const wchar_t* chkPath = redscope::report::GetReportOutPath();

    uint32_t nullRunStart = 0;
    uint32_t nullRunCount = 0;
    uint32_t boringSkipped = 0;
    auto flushNulls = [&](uint32_t endByteOffset) {
        if (nullRunCount == 0) return;
        if (nullRunCount <= 2) {
            for (uint32_t k = 0; k < nullRunCount; ++k) {
                EmitSlotPrefix(out, nullRunStart + k * 8, 0);
                out.Append("   NULL\n");
            }
        } else {
            out.Appendf("[SP+0x%04X..+0x%04X]  0x0 x%u zeroed slots\n",
                        nullRunStart, endByteOffset - 8, nullRunCount);
        }
        nullRunCount = 0;
    };

    constexpr uint32_t kAsciiBufCap = 192;
    char     asciiBuf[kAsciiBufCap + 1];
    uint32_t asciiLen      = 0;
    bool     asciiInRun    = false;
    uint32_t asciiStartOff = 0;
    uint32_t asciiLastOff  = 0;
    auto appendAscii = [&](const char* s, int n) {
        for (int k = 0; k < n && asciiLen < kAsciiBufCap; ++k) asciiBuf[asciiLen++] = s[k];
    };
    auto flushAscii = [&]() {
        if (!asciiInRun) return;
        asciiInRun = false;
        if (asciiLen < 8) { asciiLen = 0; return; }
        asciiBuf[asciiLen] = '\0';
        if (asciiStartOff == asciiLastOff) {
            if (asciiStartOff <= 0xFFFF) out.Appendf("[SP+0x%04X]  ASCII \"%s\"\n", asciiStartOff, asciiBuf);
            else                         out.Appendf("[SP+0x%05X]  ASCII \"%s\"\n", asciiStartOff, asciiBuf);
        } else if (asciiLastOff <= 0xFFFF) {
            out.Appendf("[SP+0x%04X..+0x%04X]  ASCII \"%s\"\n", asciiStartOff, asciiLastOff, asciiBuf);
        } else {
            out.Appendf("[SP+0x%05X..+0x%05X]  ASCII \"%s\"\n", asciiStartOff, asciiLastOff, asciiBuf);
        }
        asciiLen = 0;
    };

    for (uint32_t i = 0; i < slotCount; ++i) {
        const uint32_t byteOffset = i * 8;
        const void* slotAddr = reinterpret_cast<const void*>(rsp + byteOffset);

        uint64_t qword = 0;
        if (!redscope::SehSafeReadValue<uint64_t>(qword, slotAddr)) {
            flushAscii();
            flushNulls(byteOffset);
            EmitTruncation(out, byteOffset);
            out.Append("\n");
            redscope::report::CheckpointBuffer(out, chkPath);
            return;
        }

        const bool atCheckpoint = chkPath && (((i + 1) % kCheckpointEverySlots) == 0);

        char asciiChars[8];
        int  asciiPrintable = 0;
        bool asciiSawNul = false;
        for (int b = 0; b < 8; ++b) {
            uint8_t c = (uint8_t)(qword >> (b * 8));
            if (c == 0) { asciiSawNul = true; break; }
            if (c < 0x20 || c > 0x7E) { asciiPrintable = -1; break; }
            asciiChars[asciiPrintable++] = (char)c;
        }
        const bool asciiFull = (asciiPrintable == 8 && !asciiSawNul);
        const bool asciiTerm = (asciiPrintable >= 1 && asciiSawNul);
        if (asciiInRun) {
            if (asciiFull) {
                appendAscii(asciiChars, 8); asciiLastOff = byteOffset;
                if (atCheckpoint) redscope::report::CheckpointBuffer(out, chkPath);
                continue;
            }
            if (asciiTerm) {
                appendAscii(asciiChars, asciiPrintable); asciiLastOff = byteOffset;
                flushAscii();
                if (atCheckpoint) redscope::report::CheckpointBuffer(out, chkPath);
                continue;
            }
            flushAscii();
        } else if (asciiFull) {
            flushNulls(byteOffset);
            asciiInRun = true; asciiStartOff = byteOffset; asciiLastOff = byteOffset; asciiLen = 0;
            appendAscii(asciiChars, 8);
            if (atCheckpoint) redscope::report::CheckpointBuffer(out, chkPath);
            continue;
        }

        auto info = redscope::rtti::InspectFast((uintptr_t)qword);
        if (info.kind == redscope::rtti::PointerKind::Null) {
            if (nullRunCount == 0) nullRunStart = byteOffset;
            ++nullRunCount;
            if (chkPath && ((i + 1) % kCheckpointEverySlots) == 0) {
                redscope::report::CheckpointBuffer(out, chkPath);
            }
            continue;
        }
        if (info.kind == redscope::rtti::PointerKind::OtherData ||
            info.kind == redscope::rtti::PointerKind::Unknown) {
            flushNulls(byteOffset);
            ++boringSkipped;
            if (chkPath && ((i + 1) % kCheckpointEverySlots) == 0) {
                redscope::report::CheckpointBuffer(out, chkPath);
            }
            continue;
        }
        flushNulls(byteOffset);
        EmitSlotPrefix(out, byteOffset, qword);
        switch (info.kind) {
            case redscope::rtti::PointerKind::Code:
                if (info.symbolName[0]) {
                    out.Appendf("   (Code) %s!%s+0x%X\n",
                                info.moduleName, info.symbolName, info.symbolOffset);
                } else {
                    out.Appendf("   (Code) %s + 0x%llX\n",
                                info.moduleName, (unsigned long long)info.moduleOffset);
                }
                break;
            case redscope::rtti::PointerKind::RttiObject: {
                uint32_t strong = 0, weak = 0;
                const bool probed = (i + 1 < slotCount) &&
                    ProbeHandleRefBlock(rsp + byteOffset + 8, strong, weak);

                const redscope::Snapshot* snap = redscope::snap::Current();
                const redscope::snap::ClassWithScriptedFields* fieldsCls =
                    snap ? redscope::snap::FindScriptedFieldClass(
                               snap->rttiSnapshot, info.className) : nullptr;
                char annot[48] = {};
                if (fieldsCls) {
                    std::snprintf(annot, sizeof(annot),
                                  "  [+%u mod fields]", fieldsCls->scriptedFieldCount);
                }

                if (probed) {
                    out.Appendf("   (Handle<%s>) strong=%u weak=%u%s\n",
                                info.className, strong, weak, annot);
                } else {
                    out.Appendf("   (Object) %s%s\n", info.className, annot);
                }
                break;
            }
            case redscope::rtti::PointerKind::String:
                out.Appendf("   (String) \"%s\"\n", info.className);
                break;
            case redscope::rtti::PointerKind::CName:
                out.Appendf("   CName(%s)\n", info.className);
                break;
            case redscope::rtti::PointerKind::TweakDBID:
                out.Appendf("   %s\n", info.className);
                break;
            case redscope::rtti::PointerKind::Sentinel:
                out.Appendf("   %s\n", info.className);
                break;
            default:
                break;
        }

        if (chkPath && ((i + 1) % kCheckpointEverySlots) == 0) {
            redscope::report::CheckpointBuffer(out, chkPath);
        }
    }
    flushAscii();
    flushNulls(slotCount * 8);
    if (boringSkipped > 0) {
        out.Appendf("(%u data/other stack slots omitted)\n", boringSkipped);
    }
    out.Append("\n");
    redscope::report::CheckpointBuffer(out, chkPath);
}

}
