#include <gtest/gtest.h>
#include "../src/Config.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

static fs::path WriteIni(const std::string& body) {
    auto dir = fs::temp_directory_path() / "redscope_test_config";
    fs::create_directories(dir);
    auto p = dir / "REDscope.ini";
    std::ofstream(p) << body;
    return dir;
}

TEST(Config, DefaultsWhenFileMissing) {
    auto dir = fs::temp_directory_path() / "redscope_test_config_missing";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto cfg = redscope::LoadConfig(dir);
    EXPECT_TRUE(cfg.respectDebugger);
    EXPECT_EQ(cfg.breadcrumbBufferSize, 256u);
    EXPECT_EQ(cfg.stackDumpSlotCount, 512u);
    EXPECT_EQ(cfg.testInjectCrashAfterSeconds, 0u);
}

TEST(Config, ParsesAllKeys) {
    auto dir = WriteIni(R"(
[core]
respect_debugger = false
terminate_process_after_report = true
capture_all_thread_stacks = false
max_crash_files = 10
[buffers]
breadcrumb_buffer_size = 512
find_entity_ring_size = 64
tweakdb_ring_size = 8
archive_ring_size = 8
snapshot_interval_ms = 250
[symbols]
symbol_db_path = symbols/foo.json
[stack]
dump_slot_count = 1024
[test]
test_inject_crash_after_seconds = 5
)");
    auto cfg = redscope::LoadConfig(dir);
    EXPECT_FALSE(cfg.respectDebugger);
    EXPECT_TRUE(cfg.terminateProcessAfterReport);
    EXPECT_FALSE(cfg.captureAllThreadStacks);
    EXPECT_EQ(cfg.maxCrashFiles, 10u);
    EXPECT_EQ(cfg.breadcrumbBufferSize, 512u);
    EXPECT_EQ(cfg.snapshotIntervalMs, 250u);
    EXPECT_EQ(cfg.stackDumpSlotCount, 1024u);
    EXPECT_EQ(cfg.testInjectCrashAfterSeconds, 5u);
    EXPECT_EQ(cfg.symbolDbPath, dir / "symbols" / "foo.json");
}

TEST(Config, BadValueFallsBackToDefault) {
    auto dir = WriteIni("[core]\nmax_crash_files = not_a_number\n");
    auto cfg = redscope::LoadConfig(dir);
    EXPECT_EQ(cfg.maxCrashFiles, 50u);
}
