#include <gtest/gtest.h>
#include "../src/report/RsdbResolver.h"
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using redscope::report::RsdbDict;

namespace {

void PutU32LE(std::vector<unsigned char>& b, uint32_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
}

void PutU64BE(std::vector<unsigned char>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        b.push_back(static_cast<unsigned char>((v >> (i * 8)) & 0xFF));
    }
}

struct Pair { uint64_t hash; std::string path; };

std::filesystem::path WriteFixture(const char* name, const std::vector<Pair>& sorted) {
    std::vector<unsigned char> index;
    std::vector<unsigned char> paths;
    for (const auto& p : sorted) {
        PutU64BE(index, p.hash);
        PutU32LE(index, static_cast<uint32_t>(paths.size()));
        PutU32LE(index, static_cast<uint32_t>(p.path.size()));
        paths.insert(paths.end(), p.path.begin(), p.path.end());
    }
    const uint32_t headerSize  = 16;
    const uint32_t indexOffset = headerSize;
    const uint32_t pathsOffset = headerSize + static_cast<uint32_t>(index.size());

    std::vector<unsigned char> file;
    file.push_back('R'); file.push_back('S'); file.push_back('D'); file.push_back('1');
    PutU32LE(file, static_cast<uint32_t>(sorted.size()));
    PutU32LE(file, indexOffset);
    PutU32LE(file, pathsOffset);
    file.insert(file.end(), index.begin(), index.end());
    file.insert(file.end(), paths.begin(), paths.end());

    auto path = std::filesystem::temp_directory_path() / name;
    std::FILE* f = nullptr;
    _wfopen_s(&f, path.wstring().c_str(), L"wb");
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    return path;
}

}

TEST(RsdbResolver, ResolvesKnownHashes) {
    std::vector<Pair> pairs = {
        {0x0000000000000001ULL, "base\\a.mesh"},
        {0x00000000DEADBEEFULL, "base\\quest\\b.ent"},
        {0xDB8DF92FE7746657ULL, "archive\\pc\\mod\\c.xbm"},
        {0xFFFFFFFFFFFFFFFFULL, "base\\z.app"},
    };
    auto path = WriteFixture("redscope_test_ok.rsdb", pairs);
    {
        RsdbDict dict;
        ASSERT_TRUE(dict.Open(path));
        EXPECT_EQ(dict.Count(), 4u);

        std::string out;
        ASSERT_TRUE(dict.Lookup(0xDB8DF92FE7746657ULL, out));
        EXPECT_EQ(out, "archive\\pc\\mod\\c.xbm");
        ASSERT_TRUE(dict.Lookup(0x0000000000000001ULL, out));
        EXPECT_EQ(out, "base\\a.mesh");
        ASSERT_TRUE(dict.Lookup(0x00000000DEADBEEFULL, out));
        EXPECT_EQ(out, "base\\quest\\b.ent");
        ASSERT_TRUE(dict.Lookup(0xFFFFFFFFFFFFFFFFULL, out));
        EXPECT_EQ(out, "base\\z.app");
    }
    std::filesystem::remove(path);
}

TEST(RsdbResolver, MissReturnsFalse) {
    std::vector<Pair> pairs = { {0x10ULL, "x"}, {0x20ULL, "y"}, {0x30ULL, "z"} };
    auto path = WriteFixture("redscope_test_miss.rsdb", pairs);
    {
        RsdbDict dict;
        ASSERT_TRUE(dict.Open(path));

        std::string out = "untouched";
        EXPECT_FALSE(dict.Lookup(0x15ULL, out));
        EXPECT_FALSE(dict.Lookup(0x00ULL, out));
        EXPECT_FALSE(dict.Lookup(0xFFULL, out));
    }
    std::filesystem::remove(path);
}

TEST(RsdbResolver, OpenRejectsMissingFile) {
    RsdbDict dict;
    EXPECT_FALSE(dict.Open(std::filesystem::temp_directory_path() / "redscope_nope.rsdb"));
    EXPECT_FALSE(dict.IsOpen());
}

TEST(RsdbResolver, OpenRejectsBadMagic) {
    auto bad = std::filesystem::temp_directory_path() / "redscope_bad.rsdb";
    std::FILE* f = nullptr;
    _wfopen_s(&f, bad.wstring().c_str(), L"wb");
    unsigned char junk[16] = { 'X','X','X','X', 0,0,0,0, 0,0,0,0, 0,0,0,0 };
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);

    RsdbDict dict;
    EXPECT_FALSE(dict.Open(bad));
    std::filesystem::remove(bad);
}
