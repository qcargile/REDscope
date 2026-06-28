#include <gtest/gtest.h>
#include "../src/rtti/ObjectsInFlight.h"
#include "../src/rtti/PointerType.h"
#include "../src/snapshot/Snapshot.h"
#include <windows.h>
#include <cstring>
#include <string>

using redscope::rtti::DecodeObjectsInFlight;
using redscope::rtti::DecodeObjectsOnStack;
using redscope::rtti::InFlightSet;

namespace redscope::snap { void TestSetCurrentSnapshot(const redscope::Snapshot*) noexcept; }

namespace {
struct SynthRtti {
    unsigned char      modbuf[1024];
    uint64_t           objbuf[8];
    redscope::Snapshot snap;

    SynthRtti() {
        std::memset(modbuf, 0, sizeof(modbuf));
        std::memset(objbuf, 0, sizeof(objbuf));
        snap = redscope::Snapshot{};

        const char* mangled = ".?AVTestType@@";
        std::memcpy(modbuf + 0x50, mangled, std::strlen(mangled) + 1);   // TypeDescriptor name (TD@0x40, name@+0x10)

        uint32_t sig = 1, off = 0, cd = 0, tdRva = 0x40;
        std::memcpy(modbuf + 0x100, &sig, 4);
        std::memcpy(modbuf + 0x104, &off, 4);
        std::memcpy(modbuf + 0x108, &cd, 4);
        std::memcpy(modbuf + 0x10C, &tdRva, 4);

        uint64_t colPtr = (uint64_t)(uintptr_t)(modbuf + 0x100);
        std::memcpy(modbuf + 0x1F8, &colPtr, 8);                          // vtable[-8] -> COL

        objbuf[0] = (uint64_t)(uintptr_t)(modbuf + 0x200);               // object -> vtable

        snap.moduleCount        = 1;
        snap.modules[0].base    = (uintptr_t)modbuf;
        snap.modules[0].size    = (uint32_t)sizeof(modbuf);
        snap.modules[0].nameIndex = 0;
        snap.modules[0].kind    = redscope::ModuleKind::Game;
        std::strncpy(snap.moduleNames[0], "test.exe", redscope::kModuleNameCap - 1);
    }
};
}

TEST(ObjectsInFlight, GarbageRegistersYieldEmptySet) {
    CONTEXT ctx{};
    ctx.Rcx = 0x1; ctx.Rdx = 0x2; ctx.R8 = 0x3; ctx.R9 = 0x4;
    ctx.Rax = 0x5; ctx.Rbx = 0x6; ctx.Rsi = 0x7; ctx.Rdi = 0x8;
    ctx.R10 = 0x9; ctx.R11 = 0xA; ctx.R12 = 0xB; ctx.R13 = 0xC;
    ctx.R14 = 0xD; ctx.R15 = 0xE;

    InFlightSet set = DecodeObjectsInFlight(ctx);
    EXPECT_EQ(set.count, 0u);
    EXPECT_FALSE(set.Any());
    EXPECT_FALSE(set.AnyWithModFields());
}

TEST(ObjectsInFlight, ZeroRegistersYieldEmptySet) {
    CONTEXT ctx{};
    InFlightSet set = DecodeObjectsInFlight(ctx);
    EXPECT_EQ(set.count, 0u);
}

TEST(ObjectsInFlight, HelpersReflectContents) {
    InFlightSet set;
    EXPECT_FALSE(set.Any());

    set.count = 2;
    set.items[0].modFields = 0;
    set.items[1].modFields = 5;
    EXPECT_TRUE(set.Any());
    EXPECT_TRUE(set.AnyWithModFields());

    set.items[1].modFields = 0;
    EXPECT_FALSE(set.AnyWithModFields());
}

TEST(StackObjects, TryGetObjectClassNameNamesSyntheticObject) {
    SynthRtti S;
    redscope::snap::TestSetCurrentSnapshot(&S.snap);
    char name[64] = {};
    bool ok = redscope::rtti::TryGetObjectClassName((uintptr_t)S.objbuf, name, sizeof(name));
    redscope::snap::TestSetCurrentSnapshot(nullptr);
    EXPECT_TRUE(ok);
    EXPECT_NE(std::string(name).find("TestType"), std::string::npos);
}

TEST(StackObjects, DecodeObjectsOnStackFindsObject) {
    SynthRtti S;
    redscope::snap::TestSetCurrentSnapshot(&S.snap);
    uint64_t stack[512];
    std::memset(stack, 0, sizeof(stack));
    stack[7] = (uint64_t)(uintptr_t)S.objbuf;
    CONTEXT ctx{};
    ctx.Rsp = (uint64_t)(uintptr_t)stack;
    auto found = DecodeObjectsOnStack(ctx);
    redscope::snap::TestSetCurrentSnapshot(nullptr);
    ASSERT_GE(found.count, 1u);
    bool hit = false;
    for (uint32_t i = 0; i < found.count; ++i) {
        if (std::string(found.items[i].className).find("TestType") != std::string::npos) {
            hit = true;
            EXPECT_EQ(found.items[i].stackOffset, 0x38u);   // object placed at stack slot 7 (7*8)
        }
        EXPECT_STREQ(found.items[i].reg, "stk");
    }
    EXPECT_TRUE(hit);
}

TEST(InterpretNullDeref, NamesNullRegistersForSmallFault) {
    CONTEXT ctx{};
    ctx.Rax = 1; ctx.Rbx = 1; ctx.Rcx = 1; ctx.Rdx = 0; ctx.Rsi = 1; ctx.Rdi = 0;
    ctx.Rbp = 1; ctx.R8 = 1; ctx.R9 = 1; ctx.R10 = 1; ctx.R11 = 1; ctx.R12 = 1;
    ctx.R13 = 1; ctx.R14 = 1; ctx.R15 = 1;

    char out[256] = {};
    bool ok = redscope::rtti::InterpretNullDeref(ctx, 0x155, "read", out, sizeof(out));
    EXPECT_TRUE(ok);
    std::string s(out);
    EXPECT_NE(s.find("null-pointer read"), std::string::npos);
    EXPECT_NE(s.find("+0x155"),           std::string::npos);
    EXPECT_NE(s.find("RDX"),              std::string::npos);
    EXPECT_NE(s.find("RDI"),              std::string::npos);
}

TEST(InterpretNullDeref, LargeFaultAddressReturnsFalse) {
    CONTEXT ctx{};
    char out[64] = {};
    EXPECT_FALSE(redscope::rtti::InterpretNullDeref(ctx, 0x140000000ull, "read", out, sizeof(out)));
}

TEST(InterpretNullDeref, NoNullRegisterGivesSofterMessage) {
    CONTEXT ctx{};
    ctx.Rax = 1; ctx.Rbx = 1; ctx.Rcx = 1; ctx.Rdx = 1; ctx.Rsi = 1; ctx.Rdi = 1;
    ctx.Rbp = 1; ctx.R8 = 1; ctx.R9 = 1; ctx.R10 = 1; ctx.R11 = 1; ctx.R12 = 1;
    ctx.R13 = 1; ctx.R14 = 1; ctx.R15 = 1;

    char out[256] = {};
    bool ok = redscope::rtti::InterpretNullDeref(ctx, 0x40, "write", out, sizeof(out));
    EXPECT_TRUE(ok);
    std::string s(out);
    EXPECT_NE(s.find("near-null/computed"), std::string::npos);
    EXPECT_EQ(s.find("NULL register"),      std::string::npos);
}

TEST(StackObjects, GarbageStackYieldsEmpty) {
    redscope::Snapshot snap{};
    redscope::snap::TestSetCurrentSnapshot(&snap);
    uint64_t stack[512];
    for (int i = 0; i < 512; ++i) stack[i] = (uint64_t)(i * 8 + 1);
    CONTEXT ctx{};
    ctx.Rsp = (uint64_t)(uintptr_t)stack;
    auto found = DecodeObjectsOnStack(ctx);
    redscope::snap::TestSetCurrentSnapshot(nullptr);
    EXPECT_EQ(found.count, 0u);
}

TEST(StackObjects, NullRspYieldsEmpty) {
    CONTEXT ctx{};
    auto found = DecodeObjectsOnStack(ctx);
    EXPECT_EQ(found.count, 0u);
}
