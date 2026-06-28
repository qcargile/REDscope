#include "ResourceAtFault.h"
#include "../snapshot/Snapshot.h"
#include "../util/SehGuardedRead.h"

#include <cstring>

namespace redscope::report {

namespace {

constexpr uintptr_t kTokenPathOff   = 0x48;          // ResourceToken pathHash offset
constexpr uintptr_t kRefPathOff     = 0x00;          // ResourceReference embedded path offset
constexpr uintptr_t kTokenProbeSize = 0x50;          // covers up to +0x48
constexpr uintptr_t kPtrFloor       = 0x10000;       // below this is not a user pointer
constexpr uintptr_t kPtrCeil        = 0x7FFFFFFFFFFFull;
constexpr uint64_t  kSentinel       = 0xFFFFFFFFFFFFFFFFull;

const snap::ResourceLoaderSnapshot::Entry* FindKnown(
        const snap::ResourceLoaderSnapshot& known, uint64_t hash) noexcept {
    if (hash == 0 || hash == kSentinel) return nullptr;
    for (uint32_t i = 0; i < known.entryCount; ++i) {
        if (known.entries[i].pathHash == hash) return &known.entries[i];
    }
    return nullptr;
}

bool AlreadyHave(const ResourceAtFaultResult& r, uint64_t hash) noexcept {
    for (uint32_t i = 0; i < r.count; ++i) {
        if (r.entries[i].pathHash == hash) return true;
    }
    return false;
}

void Record(ResourceAtFaultResult& out, const snap::ResourceLoaderSnapshot::Entry& e,
            bool viaPointer, const char* reg) noexcept {
    if (AlreadyHave(out, e.pathHash)) return;
    if (out.count >= kMaxResourceAtFault) return;
    ResourceAtFaultEntry& dst = out.entries[out.count++];
    dst.pathHash   = e.pathHash;
    dst.failed     = e.failed;
    dst.inFlight   = e.inFlight;
    dst.viaPointer = viaPointer;
    if (reg) {
        for (int i = 0; i < 3 && reg[i]; ++i) dst.reg[i] = reg[i];
    }
}

bool LooksLikePointer(uintptr_t v) noexcept {
    return v >= kPtrFloor && v < kPtrCeil;
}

}

void ScanResourceAtFault(const RegSlot* regs, size_t regCount,
                         const uint64_t* stack, size_t stackCount,
                         const snap::ResourceLoaderSnapshot& known,
                         const snap::ResourceLoaderMemReader& mem,
                         ResourceAtFaultResult& out) noexcept {
    out = ResourceAtFaultResult{};
    out.scanned = true;
    if (known.entryCount == 0) return;

    for (size_t i = 0; i < regCount; ++i) {
        const uintptr_t v = regs[i].value;
        if (const auto* e = FindKnown(known, static_cast<uint64_t>(v))) {
            Record(out, *e, false, regs[i].name);
            continue;
        }
        if (!LooksLikePointer(v)) continue;
        if (!mem.Readable(reinterpret_cast<const void*>(v), kTokenProbeSize)) continue;
        uint64_t p0 = 0, p48 = 0;
        mem.Read(reinterpret_cast<const void*>(v + kRefPathOff),   &p0,  sizeof(p0));
        mem.Read(reinterpret_cast<const void*>(v + kTokenPathOff), &p48, sizeof(p48));
        if (const auto* e = FindKnown(known, p48)) { Record(out, *e, true, regs[i].name); continue; }
        if (const auto* e = FindKnown(known, p0))  { Record(out, *e, true, regs[i].name); }
    }

    for (size_t i = 0; i < stackCount; ++i) {
        if (const auto* e = FindKnown(known, stack[i])) {
            ++out.stackHits;
            Record(out, *e, false, nullptr);
        }
    }
}

namespace {

struct LiveResMemReader : snap::ResourceLoaderMemReader {
    bool Read(const void* addr, void* dst, size_t bytes) const noexcept override {
        return redscope::SehSafeRead(dst, addr, bytes);
    }
    bool Readable(const void* addr, size_t bytes) const noexcept override {
        return redscope::LooksReadable(addr, bytes);
    }
};

ResourceAtFaultResult g_last;

}

ResourceAtFaultResult DecodeResourceAtFault(const CONTEXT& ctx, const Snapshot* snapshot) noexcept {
    ResourceAtFaultResult out{};
    out.scanned = true;
    if (!snapshot || !snapshot->resourceLoader.readOk) return out;
    const auto& known = snapshot->resourceLoader;

    const RegSlot regs[] = {
        {{'R','C','X',0}, static_cast<uintptr_t>(ctx.Rcx)},
        {{'R','D','X',0}, static_cast<uintptr_t>(ctx.Rdx)},
        {{'R','8',0,0},   static_cast<uintptr_t>(ctx.R8 )},
        {{'R','9',0,0},   static_cast<uintptr_t>(ctx.R9 )},
        {{'R','A','X',0}, static_cast<uintptr_t>(ctx.Rax)},
        {{'R','B','X',0}, static_cast<uintptr_t>(ctx.Rbx)},
        {{'R','S','I',0}, static_cast<uintptr_t>(ctx.Rsi)},
        {{'R','D','I',0}, static_cast<uintptr_t>(ctx.Rdi)},
        {{'R','1','0',0}, static_cast<uintptr_t>(ctx.R10)},
        {{'R','1','1',0}, static_cast<uintptr_t>(ctx.R11)},
        {{'R','1','2',0}, static_cast<uintptr_t>(ctx.R12)},
        {{'R','1','3',0}, static_cast<uintptr_t>(ctx.R13)},
        {{'R','1','4',0}, static_cast<uintptr_t>(ctx.R14)},
        {{'R','1','5',0}, static_cast<uintptr_t>(ctx.R15)},
    };

    constexpr size_t kStackSlots = 512;                  // stack window from RSP
    uint64_t stack[kStackSlots];
    size_t readable = 0;
    const uintptr_t rsp = static_cast<uintptr_t>(ctx.Rsp);
    if (rsp != 0) {
        if (redscope::SehSafeRead(stack, reinterpret_cast<const void*>(rsp), sizeof(stack))) {
            readable = kStackSlots;
        } else {
            for (; readable < kStackSlots; ++readable) {
                const void* slot = reinterpret_cast<const void*>(rsp + static_cast<uintptr_t>(readable) * 8u);
                if (!redscope::SehSafeReadValue<uint64_t>(stack[readable], slot)) break;
            }
        }
    }

    LiveResMemReader mem;
    ScanResourceAtFault(regs, sizeof(regs) / sizeof(regs[0]), stack, readable, known, mem, out);
    return out;
}

void SetLastResourceAtFault(const ResourceAtFaultResult& r) noexcept { g_last = r; }
const ResourceAtFaultResult& GetLastResourceAtFault() noexcept { return g_last; }

}
