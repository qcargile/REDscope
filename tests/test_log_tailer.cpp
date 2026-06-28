#include <gtest/gtest.h>
#include "../src/logs/LogTailer.h"
#include <fstream>
#include <chrono>
#include <ctime>
#include <random>
#include <thread>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

class LogTailerTest : public ::testing::Test {
protected:
    fs::path base;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        char name[64];
        std::snprintf(name, sizeof(name), "redscope_logtailer_%llx",
                      (unsigned long long)gen());
        base = fs::temp_directory_path() / name;
        fs::create_directories(base);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    fs::path WriteFile(const std::string& filename, const std::string& contents) {
        fs::path p = base / filename;
        std::ofstream f(p, std::ios::binary);
        f.write(contents.data(), (std::streamsize)contents.size());
        return p;
    }

    static std::string TimestampedLine(std::chrono::system_clock::time_point anchor,
                                       int offsetSec,
                                       const std::string& body) {
        auto t = std::chrono::system_clock::to_time_t(anchor + std::chrono::seconds(offsetSec));
        std::tm tmv{};
        localtime_s(&tmv, &t);
        char ts[32];
        std::strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S", &tmv);
        return std::string(ts) + "] " + body;
    }

    static void TouchFile(const fs::path& p, FILETIME ft) {
        HANDLE h = ::CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(h, INVALID_HANDLE_VALUE);
        ASSERT_NE(::SetFileTime(h, nullptr, nullptr, &ft), 0);
        ::CloseHandle(h);
    }

    static FILETIME FiletimeFromOffset(int secOffsetFromNow) {
        SYSTEMTIME st;
        ::GetSystemTime(&st);
        FILETIME ft;
        ::SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart += (ULONGLONG)secOffsetFromNow * 10000000ULL;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        return ft;
    }
};

}

TEST_F(LogTailerTest, TailReturnsLastN) {
    auto p = WriteFile("a.log", "one\ntwo\nthree\nfour\nfive\n");
    auto lines = redscope::logs::Tail(p, 2);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "four");
    EXPECT_EQ(lines[1], "five");
}

TEST_F(LogTailerTest, TailHandlesNoTrailingNewline) {
    auto p = WriteFile("b.log", "one\ntwo\nthree");
    auto lines = redscope::logs::Tail(p, 5);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "one");
    EXPECT_EQ(lines[1], "two");
    EXPECT_EQ(lines[2], "three");
}

TEST_F(LogTailerTest, TailEmptyFileReturnsEmpty) {
    auto p = WriteFile("c.log", "");
    auto lines = redscope::logs::Tail(p, 10);
    EXPECT_TRUE(lines.empty());
}

TEST_F(LogTailerTest, TailMissingFileReturnsEmpty) {
    auto lines = redscope::logs::Tail(base / "does-not-exist.log", 10);
    EXPECT_TRUE(lines.empty());
}

TEST_F(LogTailerTest, WindowFiltersByTimestamp) {
    auto crash = std::chrono::system_clock::now();
    std::string contents;
    contents += TimestampedLine(crash, -50, "older - out of window") + "\n";
    contents += TimestampedLine(crash, -20, "in window before crash") + "\n";
    contents += TimestampedLine(crash,   0, "AT crash") + "\n";
    contents += TimestampedLine(crash, +25, "in window after crash") + "\n";

    auto p = WriteFile("w.log", contents);
    auto lines = redscope::logs::WindowAround(p, crash, 30, 100);

    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("in window before crash"), std::string::npos);
    EXPECT_NE(lines[1].find("AT crash"), std::string::npos);
    EXPECT_NE(lines[2].find("in window after crash"), std::string::npos);
}

TEST_F(LogTailerTest, WindowKeepsContinuationLinesBetweenTimestamps) {
    auto crash = std::chrono::system_clock::now();
    std::string contents;
    contents += TimestampedLine(crash, -10, "ERROR: boom") + "\n";
    contents += "    at frame_a (foo.reds:12)\n";
    contents += "    at frame_b (bar.reds:34)\n";
    contents += "    at frame_c (baz.reds:56)\n";

    auto p = WriteFile("multi.log", contents);
    auto lines = redscope::logs::WindowAround(p, crash, 30, 100);

    ASSERT_EQ(lines.size(), 4u);
    EXPECT_NE(lines[0].find("ERROR: boom"), std::string::npos);
    EXPECT_EQ(lines[1], "    at frame_a (foo.reds:12)");
    EXPECT_EQ(lines[2], "    at frame_b (bar.reds:34)");
    EXPECT_EQ(lines[3], "    at frame_c (baz.reds:56)");
}

TEST_F(LogTailerTest, WindowCapsToMaxLines) {
    auto crash = std::chrono::system_clock::now();
    std::string contents;
    for (int i = 0; i < 100; ++i) {
        contents += TimestampedLine(crash, -50 + i, "line " + std::to_string(i)) + "\n";
    }

    auto p = WriteFile("cap.log", contents);
    auto lines = redscope::logs::WindowAround(p, crash, 60, 10);

    ASSERT_EQ(lines.size(), 10u);
    EXPECT_NE(lines[0].find("line 90"), std::string::npos);
    EXPECT_NE(lines[9].find("line 99"), std::string::npos);
}

TEST_F(LogTailerTest, FindNewestMatchingReturnsCorrectFile) {
    auto a = WriteFile("red4ext-2026-04-15.log", "old\n");
    auto b = WriteFile("red4ext-2026-04-16.log", "mid\n");
    auto c = WriteFile("red4ext-2026-04-17.log", "new\n");
    WriteFile("other.log", "ignored\n");

    TouchFile(a, FiletimeFromOffset(-1000));
    TouchFile(b, FiletimeFromOffset(-500));
    TouchFile(c, FiletimeFromOffset(-100));

    auto found = redscope::logs::FindNewestMatching(base, "red4ext-*.log");
    EXPECT_EQ(found.filename().string(), "red4ext-2026-04-17.log");
}

TEST_F(LogTailerTest, FindNewestMatchingNoMatchReturnsEmpty) {
    WriteFile("other.log", "x\n");
    auto found = redscope::logs::FindNewestMatching(base, "red4ext-*.log");
    EXPECT_TRUE(found.empty());
}

TEST_F(LogTailerTest, WindowOnTimestamplessLogReturnsEmpty) {
    auto p = WriteFile("cet-style.log",
        "[INFO][CET][LuaVM] mod loaded\n"
        "[INFO][CET][LuaVM] init complete\n"
        "[ERROR][CET][LuaVM] something broke\n");

    auto crash = std::chrono::system_clock::now();
    auto lines = redscope::logs::WindowAround(p, crash, 60, 100);
    EXPECT_TRUE(lines.empty());

    auto tail = redscope::logs::Tail(p, 100);
    ASSERT_EQ(tail.size(), 3u);
}

TEST_F(LogTailerTest, TailHandlesCRLF) {
    auto p = WriteFile("crlf.log", "alpha\r\nbeta\r\ngamma\r\n");
    auto lines = redscope::logs::Tail(p, 5);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "alpha");
    EXPECT_EQ(lines[1], "beta");
    EXPECT_EQ(lines[2], "gamma");
    for (const auto& ln : lines) {
        EXPECT_EQ(ln.find('\r'), std::string::npos);
    }
}
