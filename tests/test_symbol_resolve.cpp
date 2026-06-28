#include <gtest/gtest.h>
#include "../src/symbols/SymbolDb.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstring>

namespace {

bool g_dbgHelpInitialized = false;

void EnsureDbgHelp() {
    if (g_dbgHelpInitialized) return;
    ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
    g_dbgHelpInitialized = true;
}

int __declspec(noinline) SymbolResolveProbeFunction(int x) {
    return x * 2 + 1;
}

}

TEST(SymbolResolve, FindsOwnFunctionByAddress) {
    EnsureDbgHelp();
    char nameBuf[256] = {};
    uint32_t off = 0;
    bool found = redscope::symbols::LookupByAddress(
        reinterpret_cast<uintptr_t>(&SymbolResolveProbeFunction),
        nameBuf, sizeof(nameBuf), off);
    ASSERT_TRUE(found) << "DbgHelp should resolve a non-static, non-inline function in the test binary";
    ASSERT_GT(std::strlen(nameBuf), 0u);
    EXPECT_NE(std::strstr(nameBuf, "SymbolResolveProbeFunction"), nullptr)
        << "Expected the demangled name to contain SymbolResolveProbeFunction, got: " << nameBuf;
}

TEST(SymbolResolve, ReturnsFalseForNullAddress) {
    EnsureDbgHelp();
    char nameBuf[64] = {};
    uint32_t off = 0xDEADBEEF;
    bool found = redscope::symbols::LookupByAddress(0, nameBuf, sizeof(nameBuf), off);
    EXPECT_FALSE(found);
    EXPECT_EQ(nameBuf[0], '\0');
    EXPECT_EQ(off, 0u);
}

TEST(SymbolResolve, ResolveCodeAddressFallsBackToDbgHelpForNonGameModules) {
    EnsureDbgHelp();
    char nameBuf[256] = {};
    uint32_t off = 0;
    bool found = redscope::symbols::ResolveCodeAddress(
        "redscope_tests.exe",
        reinterpret_cast<uintptr_t>(&SymbolResolveProbeFunction),
        0,
        nameBuf, sizeof(nameBuf), off);
    ASSERT_TRUE(found) << "Non-Cyberpunk2077.exe modules must fall through to DbgHelp resolution";
    EXPECT_NE(std::strstr(nameBuf, "SymbolResolveProbeFunction"), nullptr);
}
