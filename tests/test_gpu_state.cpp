#include <gtest/gtest.h>
#include "../src/snapshot/GpuState.h"

#include <cstring>
#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;
using redscope::snap::CaptureInto;
using redscope::snap::GpuState;
using redscope::snap::kGpuHintCount;

namespace {

fs::path MakeTempDir() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char name[64];
    std::snprintf(name, sizeof(name), "redscope_gpustate_%llx",
                  (unsigned long long)gen());
    fs::path p = fs::temp_directory_path() / name;
    fs::create_directories(p);
    return p;
}

}

TEST(GpuState, CaptureFillsKnownLabels) {
    GpuState g{};
    CaptureInto(g, fs::path{});

    std::string joinedLabels;
    for (size_t i = 0; i < kGpuHintCount; ++i) {
        if (g.hintLabels[i][0]) {
            joinedLabels += g.hintLabels[i];
            joinedLabels += '|';
        }
    }
    EXPECT_NE(joinedLabels.find("NVIDIA Aftermath"),    std::string::npos);
    EXPECT_NE(joinedLabels.find("Windows CrashDumps"),  std::string::npos);
}

TEST(GpuState, CaptureSkipsGameBinSlotWhenRootEmpty) {
    GpuState g{};
    CaptureInto(g, fs::path{});

    bool sawGameSlot = false;
    for (size_t i = 0; i < kGpuHintCount; ++i) {
        if (std::strcmp(g.hintLabels[i], "Game bin/x64") == 0) sawGameSlot = true;
    }
    EXPECT_FALSE(sawGameSlot);
}

TEST(GpuState, CaptureIncludesGameBinSlotWhenRootProvided) {
    GpuState g{};
    fs::path fakeRoot = "C:\\Games\\Cyberpunk 2077";
    CaptureInto(g, fakeRoot);

    bool sawGameSlot = false;
    for (size_t i = 0; i < kGpuHintCount; ++i) {
        if (std::strcmp(g.hintLabels[i], "Game bin/x64") == 0) {
            sawGameSlot = true;
            EXPECT_NE(std::strstr(g.hintPaths[i], "bin"), nullptr);
            EXPECT_NE(std::strstr(g.hintPaths[i], "x64"), nullptr);
        }
    }
    EXPECT_TRUE(sawGameSlot);
}

TEST(GpuState, HintExistsReflectsFilesystem) {
    auto tmp = MakeTempDir();
    GpuState g{};
    CaptureInto(g, tmp);

    bool matched = false;
    for (size_t i = 0; i < kGpuHintCount; ++i) {
        if (std::strcmp(g.hintLabels[i], "Game bin/x64") == 0) {
            matched = true;
            EXPECT_FALSE(g.hintExists[i]);
        }
    }
    ASSERT_TRUE(matched);

    fs::create_directories(tmp / "bin" / "x64");
    GpuState g2{};
    CaptureInto(g2, tmp);
    for (size_t i = 0; i < kGpuHintCount; ++i) {
        if (std::strcmp(g2.hintLabels[i], "Game bin/x64") == 0) {
            EXPECT_TRUE(g2.hintExists[i]);
        }
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}
