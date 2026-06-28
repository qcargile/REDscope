#include <gtest/gtest.h>
#include "../src/rtti/FaultInstruction.h"
#include <windows.h>
#include <cstdint>
#include <string>

using redscope::rtti::DecodeFaultOperand;
using redscope::rtti::DecodedFault;

TEST(FaultInstruction, MovByteFromNullRdx) {
    uint8_t code[] = {0x44, 0x8a, 0x82, 0x55, 0x01, 0x00, 0x00, 0,0,0,0,0,0,0,0,0}; // mov r8b,[rdx+0x155]
    CONTEXT ctx{};
    ctx.Rdx = 0;
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0x155);
    EXPECT_TRUE(d.ok);
    EXPECT_TRUE(d.eaMatches);
    EXPECT_TRUE(d.baseHasReg);
    EXPECT_STREQ(d.baseReg, "RDX");
    EXPECT_EQ(d.disp, 0x155);
    EXPECT_TRUE(d.baseIsNull);
    EXPECT_NE(std::string(d.operand).find("RDX"),   std::string::npos);
    EXPECT_NE(std::string(d.operand).find("0x155"), std::string::npos);
}

TEST(FaultInstruction, EaMismatchWhenRegistersDiffer) {
    uint8_t code[] = {0x44, 0x8a, 0x82, 0x55, 0x01, 0x00, 0x00, 0,0,0,0,0,0,0,0,0}; // mov r8b,[rdx+0x155]
    CONTEXT ctx{};
    ctx.Rdx = 0x1000;
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0x155);
    EXPECT_TRUE(d.ok);
    EXPECT_FALSE(d.eaMatches);
}

TEST(FaultInstruction, Disp8NonNullBase) {
    uint8_t code[] = {0x48, 0x8b, 0x48, 0x10, 0,0,0,0,0,0,0,0,0,0,0,0}; // mov rcx,[rax+0x10]
    CONTEXT ctx{};
    ctx.Rax = 0xDEAD0000;
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0xDEAD0010);
    EXPECT_TRUE(d.ok);
    EXPECT_TRUE(d.eaMatches);
    EXPECT_STREQ(d.baseReg, "RAX");
    EXPECT_EQ(d.disp, 0x10);
    EXPECT_FALSE(d.baseIsNull);
}

TEST(FaultInstruction, SibBaseIndexScale) {
    uint8_t code[] = {0x8b, 0x04, 0x88, 0,0,0,0,0,0,0,0,0,0,0,0,0}; // mov eax,[rax+rcx*4]
    CONTEXT ctx{};
    ctx.Rax = 0x2000;
    ctx.Rcx = 0x4;
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0x2010);
    EXPECT_TRUE(d.ok);
    EXPECT_TRUE(d.eaMatches);
    EXPECT_STREQ(d.baseReg, "RAX");
    EXPECT_TRUE(d.indexHasReg);
    EXPECT_STREQ(d.indexReg, "RCX");
    EXPECT_EQ(d.scale, 4);
}

TEST(FaultInstruction, RexBExtendsBaseToR8) {
    uint8_t code[] = {0x41, 0x8b, 0x00, 0,0,0,0,0,0,0,0,0,0,0,0,0}; // mov eax,[r8]
    CONTEXT ctx{};
    ctx.R8 = 0x5000;
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0x5000);
    EXPECT_TRUE(d.ok);
    EXPECT_TRUE(d.eaMatches);
    EXPECT_STREQ(d.baseReg, "R8");
}

TEST(FaultInstruction, RipRelativeHasNoGprBaseAndNoEaMatch) {
    uint8_t code[] = {0x48, 0x8b, 0x05, 0x10, 0x00, 0x00, 0x00, 0,0,0,0,0,0,0,0,0}; // mov rax,[rip+0x10]
    CONTEXT ctx{};
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0x1234);
    EXPECT_TRUE(d.ok);
    EXPECT_TRUE(d.ripRelative);
    EXPECT_FALSE(d.eaMatches);
    EXPECT_FALSE(d.baseHasReg);
    EXPECT_NE(std::string(d.operand).find("RIP"), std::string::npos);
}

TEST(FaultInstruction, RegisterDirectIsNotMemory) {
    uint8_t code[] = {0x48, 0x89, 0xc1, 0,0,0,0,0,0,0,0,0,0,0,0,0}; // mov rcx,rax (mod=3)
    CONTEXT ctx{};
    auto d = DecodeFaultOperand(code, sizeof(code), ctx, 0);
    EXPECT_FALSE(d.ok);
}
