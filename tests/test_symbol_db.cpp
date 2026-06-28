#include <gtest/gtest.h>
#include "../src/symbols/SymbolDb.h"
#include <fstream>
#include <filesystem>
#include <random>
#include <chrono>
#include <thread>
#include <cstdio>

namespace fs = std::filesystem;
namespace sym = redscope::symbols;

namespace {

class SymbolDbTest : public ::testing::Test {
protected:
    fs::path base;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        char name[64];
        std::snprintf(name, sizeof(name), "redscope_symboldb_%llx",
                      (unsigned long long)gen());
        base = fs::temp_directory_path() / name;
        fs::create_directories(base / "bin" / "x64");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    void WriteSyntheticDb() {
        fs::path p = base / "bin" / "x64" / "cyberpunk2077_addresses.json";
        std::ofstream f(p, std::ios::binary);
        f <<
R"({"Linker map timestamp":"test","Preferred load address":"0000000140000000","Code constant offset":"1000","Addresses":[{"hash":"1","secondary hash":"aa","symbol":"FuncAlpha","offset":"0001:00001000"},{"hash":"2","secondary hash":"bb","symbol":"FuncBeta","offset":"0001:00002000"},{"hash":"3","secondary hash":"cc","symbol":"FuncGamma","offset":"0001:00003500"},{"hash":"4","secondary hash":"dd","symbol":"FuncDelta","offset":"0001:00010000"},{"hash":"5","secondary hash":"ee","symbol":"red::memory::Allocator::Allocate","offset":"0001:00020000"}]})";
    }

    bool WaitForReady(int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (sym::Ready()) return true;
            const char* st = sym::Status();
            if (std::strcmp(st, "parse_error") == 0 ||
                std::strcmp(st, "missing") == 0) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return sym::Ready();
    }
};

}

TEST_F(SymbolDbTest, FullLifecycle) {
    ASSERT_FALSE(sym::Ready());
    {
        auto r = sym::Lookup(0x2000);
        EXPECT_FALSE(r.found);
    }

    EXPECT_STREQ(sym::Status(), "idle");

    WriteSyntheticDb();
    sym::StartLoad(base);
    ASSERT_TRUE(WaitForReady(10'000)) << "SymbolDb failed to become ready; status=" << sym::Status();
    EXPECT_STREQ(sym::Status(), "ready");
    EXPECT_EQ(sym::EntryCount(), 5u);

    {
        auto r = sym::Lookup(0x2000);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "FuncAlpha");
        EXPECT_EQ(r.offsetIntoFunction, 0u);
    }

    {
        auto r = sym::Lookup(0x3000);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "FuncBeta");
        EXPECT_EQ(r.offsetIntoFunction, 0u);
    }

    {
        auto r = sym::Lookup(0x3800);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "FuncBeta");
        EXPECT_EQ(r.offsetIntoFunction, 0x800u);
    }

    {
        auto r = sym::Lookup(0x10000);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "FuncGamma");
        EXPECT_EQ(r.offsetIntoFunction, 0x10000u - 0x4500u);
    }

    {
        auto r = sym::Lookup(0x1800);
        EXPECT_FALSE(r.found);
    }

    {
        auto r = sym::Lookup(0x500);
        EXPECT_FALSE(r.found);
    }

    {
        auto r = sym::Lookup(0x21000);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "red::memory::Allocator::Allocate");
        EXPECT_EQ(r.offsetIntoFunction, 0u);
    }

    {
        auto r = sym::Lookup(0x30000);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "red::memory::Allocator::Allocate");
        EXPECT_EQ(r.offsetIntoFunction, 0x30000u - 0x21000u);
    }
}

TEST_F(SymbolDbTest, SkipsUnnamedEntries) {
    fs::path p = base / "bin" / "x64" / "cyberpunk2077_addresses.json";
    std::ofstream f(p, std::ios::binary);
    f <<
R"({"Linker map timestamp":"test","Preferred load address":"0000000140000000","Code constant offset":"1000","Addresses":[{"hash":"1","secondary hash":"aa","symbol":"FuncAlpha","offset":"0001:00001000"},{"hash":"2","secondary hash":"bb","offset":"0001:00002000"},{"hash":"3","secondary hash":"cc","symbol":"FuncGamma","offset":"0001:00003500"}]})";
    f.close();

    sym::StartLoad(base);
    ASSERT_TRUE(WaitForReady(10'000)) << "SymbolDb failed to become ready; status=" << sym::Status();
    EXPECT_STREQ(sym::Status(), "ready");
    EXPECT_EQ(sym::EntryCount(), 2u);

    {
        auto r = sym::Lookup(0x2000);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "FuncAlpha");
    }
    {
        auto r = sym::Lookup(0x4500);
        ASSERT_TRUE(r.found);
        EXPECT_EQ(std::string(r.symbol), "FuncGamma");
    }
}
