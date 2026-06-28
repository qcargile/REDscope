#include <gtest/gtest.h>
#include "../src/snapshot/HardwareSpecs.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/report/Sections.h"
#include "../src/util/PreallocatedBuffer.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace redscope::snap {
void TestSetCurrentSnapshot(const Snapshot* s) noexcept;
}

using redscope::snap::CaptureHardwareSpecs;
using redscope::snap::GpuAdapter;
using redscope::snap::HardwareSpecs;
using redscope::snap::ParseSmbiosAndFillRam;
using redscope::snap::TranslateNvidiaDriverVersion;

TEST(NvidiaDriver, Translates581_80) {
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15.8180"), "581.80");
}

TEST(NvidiaDriver, TranslatesTypicalRange) {
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15.5612"), "556.12");
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15.6000"), "560.00");
    EXPECT_EQ(TranslateNvidiaDriverVersion("31.0.15.4617"), "546.17");
}

TEST(NvidiaDriver, PadsMinorZero) {
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15.8100"), "581.00");
}

TEST(NvidiaDriver, RejectsWrongPartCount) {
    EXPECT_EQ(TranslateNvidiaDriverVersion(""),           "");
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15"),    "");
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15.8180.9"), "");
}

TEST(NvidiaDriver, RejectsNonDigits) {
    EXPECT_EQ(TranslateNvidiaDriverVersion("32.0.15.x180"), "");
    EXPECT_EQ(TranslateNvidiaDriverVersion("abc.def.ghi.jkl"), "");
}

TEST(CaptureHardwareSpecs, FillsNonEmpty) {
    HardwareSpecs hw = CaptureHardwareSpecs();

    EXPECT_FALSE(hw.cpuName.empty()) << "cpuName should populate from registry";
    EXPECT_GT(hw.cpuLogicalCores, 0u);
    EXPECT_GE(hw.cpuPhysicalCores, 1u);
    EXPECT_LE(hw.cpuPhysicalCores, hw.cpuLogicalCores);
    EXPECT_GT(hw.ramTotalBytes, uint64_t(1024) * 1024 * 1024)
        << "ramTotalBytes should exceed 1 GB on any modern system";
    EXPECT_FALSE(hw.osProductName.empty());
    EXPECT_GT(hw.osBuild, 0u);

    EXPECT_TRUE(hw.cpuSupportsSse42);

    EXPECT_FALSE(hw.gpus.empty());
    if (!hw.gpus.empty()) {
        EXPECT_FALSE(hw.gpus[0].name.empty());
    }

    EXPECT_FALSE(hw.bootDriveMediaType.empty());
    EXPECT_FALSE(hw.bootDriveFs.empty());

    EXPECT_GT(hw.systemUptimeMs, 0u);
    EXPECT_GT(hw.processUptimeMs, 0u);
    EXPECT_GT(hw.processCreationFileTimeMs, 0u);
    EXPECT_FALSE(hw.commandLine.empty());
}

TEST(HardwareSpecs, SmbiosParseEmptyBlobIsSafe) {
    uint8_t header[8] = {0, 3, 4, 0, 0, 0, 0, 0};
    HardwareSpecs s;
    ParseSmbiosAndFillRam(header, sizeof(header), s);
    EXPECT_EQ(s.ramDimmCount, 0u);
    EXPECT_EQ(s.ramSpeedMHz, 0u);

    ParseSmbiosAndFillRam(nullptr, 0, s);
    ParseSmbiosAndFillRam(header, 0, s);
    ParseSmbiosAndFillRam(header, 4, s);
}

TEST(HardwareSpecs, SmbiosParseRecognizesDdr5Dimm) {
    std::vector<uint8_t> blob;
    blob.resize(8);
    blob[0] = 0;
    blob[1] = 3;
    blob[2] = 4;
    blob[3] = 0;

    const size_t type17Offset = 8;
    const uint8_t len = 0x22;
    blob.resize(type17Offset + len);
    uint8_t* t = blob.data() + type17Offset;
    t[0] = 17;
    t[1] = len;
    t[2] = 0x00; t[3] = 0x01;
    t[0x0C] = 0x00; t[0x0D] = 0x40;
    t[0x12] = 0x22;
    t[0x15] = 0xC0; t[0x16] = 0x12;
    t[0x17] = 1;
    t[0x1A] = 2;
    t[0x20] = 0xE0; t[0x21] = 0x15;

    const char* s1 = "Samsung";
    const char* s2 = "M425R";
    for (size_t i = 0; i < std::strlen(s1); ++i) blob.push_back((uint8_t)s1[i]);
    blob.push_back(0);
    for (size_t i = 0; i < std::strlen(s2); ++i) blob.push_back((uint8_t)s2[i]);
    blob.push_back(0);
    blob.push_back(0);

    blob.push_back(127); blob.push_back(4);
    blob.push_back(0xFF); blob.push_back(0xFF);
    blob.push_back(0); blob.push_back(0);

    uint32_t streamLen = (uint32_t)(blob.size() - 8);
    blob[4] = (uint8_t)(streamLen & 0xFF);
    blob[5] = (uint8_t)((streamLen >> 8)  & 0xFF);
    blob[6] = (uint8_t)((streamLen >> 16) & 0xFF);
    blob[7] = (uint8_t)((streamLen >> 24) & 0xFF);

    HardwareSpecs s;
    ParseSmbiosAndFillRam(blob.data(), (uint32_t)blob.size(), s);

    EXPECT_EQ(s.ramType, "DDR5");
    EXPECT_EQ(s.ramSpeedMHz, 5600u);
    EXPECT_EQ(s.ramDimmCount, 1u);
    EXPECT_EQ(s.ramManufacturer, "Samsung");
    EXPECT_EQ(s.ramPartNumber, "M425R");
}

namespace {

class EmitSystemFixture : public ::testing::Test {
protected:
    redscope::Snapshot s{};
    redscope::PreallocatedBuffer buf;

    void SetUp() override {
        buf.Reserve(64 * 1024);
        redscope::snap::TestSetCurrentSnapshot(&s);
    }
    void TearDown() override {
        redscope::snap::TestSetCurrentSnapshot(nullptr);
    }

    static HardwareSpecs MakeFullSpecs() {
        HardwareSpecs hw;
        hw.cpuName          = "Intel Core i7-13700K";
        hw.cpuBaseMHz       = 3400;
        hw.cpuMaxTurboMHz   = 5400;
        hw.cpuPhysicalCores = 16;
        hw.cpuLogicalCores  = 24;
        hw.cpuSupportsSse42  = true;
        hw.cpuSupportsAvx    = true;
        hw.cpuSupportsAvx2   = true;
        hw.cpuSupportsAvx512 = false;
        GpuAdapter g;
        g.name               = "NVIDIA GeForce RTX 4090";
        g.driverVersion      = "32.0.15.5612";
        g.vramDedicatedBytes = uint64_t(24) * 1024 * 1024 * 1024;
        g.sharedSystemBytes  = uint64_t(16) * 1024 * 1024 * 1024;
        hw.gpus.push_back(std::move(g));
        hw.ramTotalBytes    = uint64_t(64) * 1024 * 1024 * 1024;
        hw.ramAvailableBytes= uint64_t(42) * 1024 * 1024 * 1024;
        hw.ramType          = "DDR5";
        hw.ramSpeedMHz      = 5600;
        hw.ramDimmCount     = 2;
        hw.ramManufacturer  = "Samsung";
        hw.pageFileTotalBytes = uint64_t(64) * 1024 * 1024 * 1024;
        hw.pageFileAvailBytes = uint64_t(58) * 1024 * 1024 * 1024;
        hw.bootDriveFs       = "NTFS";
        hw.bootDriveMediaType= "SSD";
        hw.gameDriveLetter   = 'D';
        hw.gameDriveFs       = "NTFS";
        hw.gameDriveMediaType= "SSD";
        hw.displayWidth      = 2560;
        hw.displayHeight     = 1600;
        hw.displayRefreshHz  = 240;
        hw.displayHdrState   = "off";
        hw.osProductName    = "Windows 11 Pro";
        hw.osDisplayVersion = "23H2";
        hw.osMajor          = 10;
        hw.osMinor          = 0;
        hw.osBuild          = 22631;
        hw.osUbr            = 4460;
        hw.systemUptimeMs   = uint64_t(2) * 86400 * 1000
                            + uint64_t(14) * 3600 * 1000
                            + uint64_t(22) * 60 * 1000;
        hw.processUptimeMs  = uint64_t(10) * 1000;
        hw.processElevated  = false;
        hw.commandLine      = "Cyberpunk2077.exe --launcher-skip";
        return hw;
    }
};

}

TEST_F(EmitSystemFixture, BasicStruct) {
    s.hardware = MakeFullSpecs();

    redscope::report::EmitSystem(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("--- System"),     std::string::npos);
    EXPECT_NE(out.find("CPU:"),           std::string::npos);
    EXPECT_NE(out.find("GPU (primary)"),  std::string::npos);
    EXPECT_NE(out.find("RAM:"),           std::string::npos);
    EXPECT_NE(out.find("Page file:"),     std::string::npos);
    EXPECT_NE(out.find("Boot drive:"),    std::string::npos);
    EXPECT_NE(out.find("Game drive:"),    std::string::npos);
    EXPECT_NE(out.find("Display:"),       std::string::npos);
    EXPECT_NE(out.find("OS:"),            std::string::npos);
    EXPECT_NE(out.find("Uptime:"),        std::string::npos);
    EXPECT_NE(out.find("Process:"),       std::string::npos);
    EXPECT_NE(out.find("i7-13700K"),      std::string::npos);
    EXPECT_NE(out.find("RTX 4090"),       std::string::npos);
    EXPECT_NE(out.find("NTFS"),           std::string::npos);
    EXPECT_NE(out.find("22631"),          std::string::npos);
    EXPECT_NE(out.find("AVX2"),           std::string::npos);
    EXPECT_NE(out.find("max turbo"),      std::string::npos);
}

TEST_F(EmitSystemFixture, HandlesMissingFields) {
    s.hardware = HardwareSpecs{};

    redscope::report::EmitSystem(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("CPU:         (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("GPU:         (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("RAM:         (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("Page file:   (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("Boot drive:  (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("Game drive:  (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("Display:     (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("OS:          (unavailable)"),        std::string::npos);
    EXPECT_NE(out.find("Uptime:      (unavailable)"),        std::string::npos);
}

TEST_F(EmitSystemFixture, OmitsRamTypeWhenUnknown) {
    HardwareSpecs hw = MakeFullSpecs();
    hw.ramSpeedMHz = 0;
    hw.ramType.clear();
    hw.ramDimmCount = 0;
    hw.ramManufacturer.clear();
    hw.ramPartNumber.clear();
    s.hardware = hw;

    redscope::report::EmitSystem(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("RAM:"),        std::string::npos);
    EXPECT_NE(out.find("64 GB total"), std::string::npos);
    EXPECT_EQ(out.find("DDR"),         std::string::npos);
    EXPECT_EQ(out.find("MHz)"),        std::string::npos);
}

TEST_F(EmitSystemFixture, GoldenStringFormat) {
    s.hardware = MakeFullSpecs();

    redscope::report::EmitSystem(buf);
    std::string out(buf.Data(), buf.Size());

    const char* expected =
        "--- System --------------------------------------------------------------------\n"
        "CPU:         Intel Core i7-13700K\n"
        "             24 logical / 16 physical cores, 3.4 GHz base (5.4 GHz max turbo)\n"
        "             AVX2: yes  AVX-512: no  SSE4.2: yes\n"
        "GPU (primary):   NVIDIA GeForce RTX 4090 (driver 32.0.15.5612 = NVIDIA 556.12)\n"
        "                 24 GB VRAM dedicated, 16 GB shared\n"
        "RAM:         64 GB total, 42 GB available  (DDR5 @ 5600 MHz, 2\xC3\x97 32 GB Samsung)\n"
        "Page file:   64 GB total, 58 GB available\n"
        "Boot drive:  C: SSD (NTFS)\n"
        "Game drive:  D: SSD (NTFS)\n"
        "Display:     2560\xC3\x97""1600 @ 240 Hz  HDR: off\n"
        "OS:          Windows 11 Pro 23H2 (build 22631.4460)\n"
        "Uptime:      system 2d 14h 22m, process 0h 00m 10s\n"
        "Process:     elevated: no  cmdline: Cyberpunk2077.exe --launcher-skip\n"
        "\n";
    EXPECT_EQ(out, expected);
}

TEST_F(EmitSystemFixture, MultiGpuEmitsSecondaryBlock) {
    HardwareSpecs hw = MakeFullSpecs();
    GpuAdapter g2;
    g2.name = "AMD Radeon Graphics";
    g2.driverVersion = "32.0.11033.1004";
    g2.vramDedicatedBytes = uint64_t(512) * 1024 * 1024;
    g2.sharedSystemBytes  = uint64_t(16) * 1024 * 1024 * 1024;
    hw.gpus.push_back(std::move(g2));
    s.hardware = hw;

    redscope::report::EmitSystem(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("GPU (primary):"),   std::string::npos);
    EXPECT_NE(out.find("GPU (adapter 1):"), std::string::npos);
    EXPECT_NE(out.find("AMD Radeon"),       std::string::npos);
    EXPECT_NE(out.find("512 MB VRAM"),      std::string::npos);
}
