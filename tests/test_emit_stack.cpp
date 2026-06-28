#include <gtest/gtest.h>
#include "../src/report/Sections.h"
#include "../src/util/PreallocatedBuffer.h"
#include <windows.h>
#include <cstdint>
#include <string>

TEST(EmitStack, NoContextReturnsPlaceholder) {
    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    redscope::report::EmitStack(buf, nullptr);
    std::string s(buf.Data(), buf.Size());
    EXPECT_NE(s.find("(no exception context)"), std::string::npos);
}

TEST(EmitStack, EmitsExpectedSlotCountAndFormat) {
    alignas(8) uint64_t fakeStack[8] = {
        0,
        0x1234567890ABCDEFull,
        0,
        0xDEADBEEFCAFEBABEull,
        0, 0, 0, 0,
    };

    CONTEXT ctx{};
    ctx.Rsp = reinterpret_cast<DWORD64>(&fakeStack[0]);

    EXCEPTION_POINTERS ep{};
    ep.ContextRecord = &ctx;
    ep.ExceptionRecord = nullptr;

    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    redscope::report::EmitStack(buf, &ep, 4);

    std::string s(buf.Data(), buf.Size());
    EXPECT_NE(s.find("[SP+0x0000]"), std::string::npos);
    EXPECT_NE(s.find("NULL"), std::string::npos);
    EXPECT_NE(s.find("omitted"), std::string::npos);
    EXPECT_EQ(s.find("[SP+0x0020]"), std::string::npos);
}

TEST(EmitStack, DecodesInlineAsciiRun) {
    alignas(8) uint64_t fakeStack[8] = {
        0,
        0x6867666564636261ull,   // "abcdefgh"
        0x7365722E6C6B6A69ull,   // "ijkl.res"
        0,
        0, 0, 0, 0,
    };

    CONTEXT ctx{};
    ctx.Rsp = reinterpret_cast<DWORD64>(&fakeStack[0]);

    EXCEPTION_POINTERS ep{};
    ep.ContextRecord = &ctx;
    ep.ExceptionRecord = nullptr;

    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    redscope::report::EmitStack(buf, &ep, 8);

    std::string s(buf.Data(), buf.Size());
    EXPECT_NE(s.find("ASCII \"abcdefghijkl.res\""), std::string::npos);
}

TEST(EmitStack, StopsOnReadFault) {
    CONTEXT ctx{};
    ctx.Rsp = 0xFFFFFFFFFFFF0000ull;

    EXCEPTION_POINTERS ep{};
    ep.ContextRecord = &ctx;
    ep.ExceptionRecord = nullptr;

    redscope::PreallocatedBuffer buf;
    buf.Reserve(4096);
    redscope::report::EmitStack(buf, &ep, 100);

    std::string s(buf.Data(), buf.Size());
    EXPECT_NE(s.find("truncated at"), std::string::npos);
}
