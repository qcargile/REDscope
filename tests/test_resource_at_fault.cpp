#include <gtest/gtest.h>
#include "../src/report/ResourceAtFault.h"
#include "../src/snapshot/ResourceLoaderSnapshot.h"
#include <windows.h>
#include <cstring>
#include <cstdint>

using redscope::report::RegSlot;
using redscope::report::ResourceAtFaultResult;
using redscope::report::ScanResourceAtFault;
using redscope::report::DecodeResourceAtFault;
using redscope::snap::ResourceLoaderSnapshot;
using redscope::snap::ResourceLoaderMemReader;

namespace {

constexpr uint64_t HASH_A = 0xAAAA0000AAAA0001ull;   // failed token
constexpr uint64_t HASH_B = 0xBBBB0000BBBB0002ull;   // in-flight token

ResourceLoaderSnapshot KnownSet() {
    ResourceLoaderSnapshot s{};
    s.readOk = true;
    s.hasData = true;
    s.failedTotal = 1;
    s.inFlightTotal = 1;
    s.entries[0] = { HASH_A, true,  false, 5 };
    s.entries[1] = { HASH_B, false, true,  0 };
    s.entryCount = 2;
    return s;
}

struct BlobReader : ResourceLoaderMemReader {
    const uint8_t* lo = nullptr;
    const uint8_t* hi = nullptr;
    bool Readable(const void* addr, size_t bytes) const noexcept override {
        auto p = static_cast<const uint8_t*>(addr);
        return p >= lo && p + bytes <= hi;
    }
    bool Read(const void* addr, void* dst, size_t bytes) const noexcept override {
        if (!Readable(addr, bytes)) return false;
        std::memcpy(dst, addr, bytes);
        return true;
    }
};

struct NullReader : ResourceLoaderMemReader {
    bool Readable(const void*, size_t) const noexcept override { return false; }
    bool Read(const void*, void*, size_t) const noexcept override { return false; }
};

RegSlot Reg(const char* name, uintptr_t value) {
    RegSlot r{};
    for (int i = 0; i < 3 && name[i]; ++i) r.name[i] = name[i];
    r.value = value;
    return r;
}

}

TEST(ResourceAtFault, DirectHashInRegisterIsReported) {
    auto known = KnownSet();
    NullReader mem;
    RegSlot regs[] = { Reg("R12", (uintptr_t)HASH_A) };

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 1, nullptr, 0, known, mem, out);

    ASSERT_EQ(out.count, 1u);
    EXPECT_EQ(out.entries[0].pathHash, HASH_A);
    EXPECT_TRUE(out.entries[0].failed);
    EXPECT_FALSE(out.entries[0].viaPointer);
    EXPECT_STREQ(out.entries[0].reg, "R12");
    EXPECT_TRUE(out.scanned);
}

TEST(ResourceAtFault, TokenPointerInRegisterIsDereferenced) {
    auto known = KnownSet();
    uint8_t token[0x60] = {};
    std::memcpy(token + 0x48, &HASH_B, sizeof(HASH_B));   // ResourceToken pathHash slot

    BlobReader mem;
    mem.lo = token;
    mem.hi = token + sizeof(token);

    RegSlot regs[] = { Reg("RDX", reinterpret_cast<uintptr_t>(token)) };

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 1, nullptr, 0, known, mem, out);

    ASSERT_EQ(out.count, 1u);
    EXPECT_EQ(out.entries[0].pathHash, HASH_B);
    EXPECT_TRUE(out.entries[0].inFlight);
    EXPECT_TRUE(out.entries[0].viaPointer);
    EXPECT_STREQ(out.entries[0].reg, "RDX");
}

TEST(ResourceAtFault, TokenPointerAtRefOffsetIsDereferenced) {
    auto known = KnownSet();
    uint8_t token[0x60] = {};
    std::memcpy(token + 0x00, &HASH_A, sizeof(HASH_A));   // ResourceReference path slot

    BlobReader mem;
    mem.lo = token;
    mem.hi = token + sizeof(token);

    RegSlot regs[] = { Reg("RDI", reinterpret_cast<uintptr_t>(token)) };

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 1, nullptr, 0, known, mem, out);

    ASSERT_EQ(out.count, 1u);
    EXPECT_EQ(out.entries[0].pathHash, HASH_A);
    EXPECT_TRUE(out.entries[0].viaPointer);
}

TEST(ResourceAtFault, CapsAtMaxEntries) {
    ResourceLoaderSnapshot known{};
    known.readOk = true;
    for (uint32_t i = 0; i < 9; ++i) {
        known.entries[i] = { 0x1000000000000001ull + i, true, false, 0 };
    }
    known.entryCount = 9;

    NullReader mem;
    RegSlot regs[9];
    for (int i = 0; i < 9; ++i) regs[i] = Reg("R12", static_cast<uintptr_t>(0x1000000000000001ull + i));

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 9, nullptr, 0, known, mem, out);
    EXPECT_EQ(out.count, 8u);
}

TEST(ResourceAtFault, HashOnStackIsReportedAndCounted) {
    auto known = KnownSet();
    NullReader mem;
    uint64_t stack[] = { 0x1234, HASH_A, 0, HASH_A };

    ResourceAtFaultResult out;
    ScanResourceAtFault(nullptr, 0, stack, 4, known, mem, out);

    ASSERT_EQ(out.count, 1u);               // deduped
    EXPECT_EQ(out.entries[0].pathHash, HASH_A);
    EXPECT_EQ(out.entries[0].reg[0], '\0'); // stack-found has no register name
    EXPECT_EQ(out.stackHits, 2u);           // matched twice
}

TEST(ResourceAtFault, GarbageRegistersAndStackYieldNothing) {
    auto known = KnownSet();
    NullReader mem;
    RegSlot regs[] = { Reg("RCX", 0x1), Reg("RAX", 0xDEADBEEF) };
    uint64_t stack[] = { 0, 0xFFFFFFFFFFFFFFFFull, 0x42 };

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 2, stack, 3, known, mem, out);

    EXPECT_EQ(out.count, 0u);
    EXPECT_TRUE(out.scanned);
}

TEST(ResourceAtFault, EmptyKnownSetYieldsNothing) {
    ResourceLoaderSnapshot known{};
    known.readOk = true;
    NullReader mem;
    RegSlot regs[] = { Reg("R12", (uintptr_t)HASH_A) };

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 1, nullptr, 0, known, mem, out);

    EXPECT_EQ(out.count, 0u);
}

TEST(ResourceAtFault, SentinelAndZeroNeverMatch) {
    ResourceLoaderSnapshot known{};
    known.readOk = true;
    known.entries[0] = { 0, true, false, 0 };
    known.entries[1] = { 0xFFFFFFFFFFFFFFFFull, true, false, 0 };
    known.entryCount = 2;

    NullReader mem;
    RegSlot regs[] = { Reg("R8", 0), Reg("R9", 0xFFFFFFFFFFFFFFFFull) };

    ResourceAtFaultResult out;
    ScanResourceAtFault(regs, 2, nullptr, 0, known, mem, out);

    EXPECT_EQ(out.count, 0u);
}

TEST(ResourceAtFault, DecodeWithoutSnapshotIsEmptyButScanned) {
    CONTEXT ctx{};
    auto out = DecodeResourceAtFault(ctx, nullptr);
    EXPECT_FALSE(out.Any());
    EXPECT_TRUE(out.scanned);
}
