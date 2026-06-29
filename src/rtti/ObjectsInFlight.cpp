#include "ObjectsInFlight.h"
#include "../snapshot/Snapshot.h"
#include "../snapshot/SnapshotWorker.h"
#include "../util/FixedStr.h"
#include "../util/SehGuardedRead.h"

#include <cstring>
#include <cstdio>

namespace redscope::rtti {

namespace {

struct RegView { const char* name; uintptr_t value; };

bool AlreadyHave(const InFlightSet& set, const char* className) noexcept {
    for (uint32_t i = 0; i < set.count; ++i) {
        if (std::strcmp(set.items[i].className, className) == 0) return true;
    }
    return false;
}

}

InFlightSet DecodeObjectsInFlight(const CONTEXT& ctx) noexcept {
    InFlightSet set;

    const RegView regs[] = {
        {"RCX", (uintptr_t)ctx.Rcx}, {"RDX", (uintptr_t)ctx.Rdx},
        {"R8",  (uintptr_t)ctx.R8 }, {"R9",  (uintptr_t)ctx.R9 },
        {"RAX", (uintptr_t)ctx.Rax}, {"RBX", (uintptr_t)ctx.Rbx},
        {"RSI", (uintptr_t)ctx.Rsi}, {"RDI", (uintptr_t)ctx.Rdi},
        {"R10", (uintptr_t)ctx.R10}, {"R11", (uintptr_t)ctx.R11},
        {"R12", (uintptr_t)ctx.R12}, {"R13", (uintptr_t)ctx.R13},
        {"R14", (uintptr_t)ctx.R14}, {"R15", (uintptr_t)ctx.R15},
    };

    for (const auto& r : regs) {
        if (set.count >= kMaxInFlight) break;

        PointerInfo info = Inspect(r.value);
        if (info.kind != PointerKind::RttiObject) continue;
        if (info.className[0] == '\0') continue;
        if (AlreadyHave(set, info.className)) continue;

        InFlightObject& o = set.items[set.count];
        redscope::CopyFixed(o.reg, sizeof(o.reg), r.name);
        redscope::CopyFixed(o.className, sizeof(o.className), info.className);
        ++set.count;
    }

    return set;
}

InFlightSet DecodeObjectsOnStack(const CONTEXT& ctx) noexcept {
    InFlightSet set;
    const uintptr_t rsp = static_cast<uintptr_t>(ctx.Rsp);
    if (rsp == 0) return set;

    constexpr size_t kStackSlots = 512;          // crashing-thread stack window
    uint64_t stack[kStackSlots];
    size_t readable = 0;
    if (redscope::SehSafeRead(stack, reinterpret_cast<const void*>(rsp), sizeof(stack))) {
        readable = kStackSlots;
    } else {
        for (; readable < kStackSlots; ++readable) {
            const void* slot = reinterpret_cast<const void*>(rsp + static_cast<uintptr_t>(readable) * 8u);
            if (!redscope::SehSafeReadValue<uint64_t>(stack[readable], slot)) break;
        }
    }

    const Snapshot* snap = redscope::snap::Current();
    for (size_t i = 0; i < readable && set.count < kMaxInFlight; ++i) {
        const uintptr_t v = static_cast<uintptr_t>(stack[i]);
        if (v < 0x10000 || v >= 0x0000800000000000ull) continue;        // Windows 47-bit user-mode ceiling
        if (snap && redscope::FindModuleByPC(*snap, v)) continue;

        char cls[kPointerInfoNameCap] = {};
        if (!TryGetObjectClassName(v, cls, sizeof(cls))) continue;
        if (AlreadyHave(set, cls)) continue;

        InFlightObject& o = set.items[set.count];
        redscope::CopyFixed(o.reg, sizeof(o.reg), "stk");
        o.stackOffset = static_cast<uint32_t>(i * 8u);
        redscope::CopyFixed(o.className, sizeof(o.className), cls);
        ++set.count;
    }

    return set;
}

static InFlightSet g_lastStackObjects;
void SetLastStackObjects(const InFlightSet& set) noexcept { g_lastStackObjects = set; }
const InFlightSet& GetLastStackObjects() noexcept { return g_lastStackObjects; }

bool InterpretNullDeref(const CONTEXT& ctx, uintptr_t dataAddr, const char* op,
                        char* out, size_t cap) noexcept {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (dataAddr >= 0x10000) return false;          // not a plausible struct-field offset off a null base

    struct RV { const char* n; uint64_t v; };
    const RV gprs[] = {
        {"RAX", ctx.Rax}, {"RBX", ctx.Rbx}, {"RCX", ctx.Rcx}, {"RDX", ctx.Rdx},
        {"RSI", ctx.Rsi}, {"RDI", ctx.Rdi}, {"RBP", ctx.Rbp},
        {"R8",  ctx.R8 }, {"R9",  ctx.R9 }, {"R10", ctx.R10}, {"R11", ctx.R11},
        {"R12", ctx.R12}, {"R13", ctx.R13}, {"R14", ctx.R14}, {"R15", ctx.R15},
    };

    char nulls[80] = {};
    size_t used = 0;
    uint32_t nullCount = 0;
    for (const auto& g : gprs) {
        if (g.v != 0) continue;
        size_t n = std::strlen(g.n);
        if (used + n + 2 >= sizeof(nulls)) break;
        if (used) { nulls[used++] = ','; nulls[used++] = ' '; }
        std::memcpy(nulls + used, g.n, n);
        used += n;
        nulls[used] = '\0';
        ++nullCount;
    }

    const char* opStr = (op && op[0]) ? op : "access";
    if (nullCount > 0) {
        std::snprintf(out, cap,
                      "null-pointer %s of field +0x%llX (NULL register%s: %s)",
                      opStr, static_cast<unsigned long long>(dataAddr),
                      nullCount == 1 ? "" : "s", nulls);
    } else {
        std::snprintf(out, cap,
                      "%s at offset +0x%llX off a near-null/computed pointer (no register is exactly zero)",
                      opStr, static_cast<unsigned long long>(dataAddr));
    }
    return out[0] != '\0';
}

}
