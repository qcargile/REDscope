#include "FaultInstruction.h"

#include <cstdio>
#include <cstring>

namespace redscope::rtti {

namespace {

const char* const kRegNames[16] = {
    "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
    "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15",
};

uint64_t RegVal(const CONTEXT& c, int i) noexcept {
    switch (i) {
        case 0:  return c.Rax; case 1:  return c.Rcx; case 2:  return c.Rdx; case 3:  return c.Rbx;
        case 4:  return c.Rsp; case 5:  return c.Rbp; case 6:  return c.Rsi; case 7:  return c.Rdi;
        case 8:  return c.R8;  case 9:  return c.R9;  case 10: return c.R10; case 11: return c.R11;
        case 12: return c.R12; case 13: return c.R13; case 14: return c.R14; case 15: return c.R15;
    }
    return 0;
}

void CopyReg(char* dst, const char* src) noexcept {
    size_t i = 0;
    for (; i < 3 && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

int32_t ReadDisp32(const uint8_t* p) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

void BuildOperand(DecodedFault& d) noexcept {
    char buf[48];
    size_t n = 0;
    auto advance = [&](int written) noexcept {
        if (written < 0) return;
        size_t w = static_cast<size_t>(written);
        n = (n + w < sizeof(buf)) ? n + w : sizeof(buf) - 1;
    };

    buf[n++] = '[';
    if (d.ripRelative) {
        advance(std::snprintf(buf + n, sizeof(buf) - n, "RIP"));
    } else if (d.baseHasReg) {
        advance(std::snprintf(buf + n, sizeof(buf) - n, "%s", d.baseReg));
    }
    if (d.indexHasReg) {
        advance(std::snprintf(buf + n, sizeof(buf) - n, "%s%s*%u",
                              (n > 1 ? "+" : ""), d.indexReg, d.scale));
    }
    if (d.disp != 0 || (!d.baseHasReg && !d.indexHasReg && !d.ripRelative)) {
        if (d.disp < 0) {
            advance(std::snprintf(buf + n, sizeof(buf) - n, "-0x%llX",
                                  static_cast<unsigned long long>(-d.disp)));
        } else {
            advance(std::snprintf(buf + n, sizeof(buf) - n, "%s0x%llX",
                                  (n > 1 ? "+" : ""),
                                  static_cast<unsigned long long>(d.disp)));
        }
    }
    if (n < sizeof(buf) - 1) buf[n++] = ']';
    buf[n] = '\0';
    std::memcpy(d.operand, buf, n + 1);
}

}

DecodedFault DecodeFaultOperand(const uint8_t* code, size_t codeLen,
                                const CONTEXT& ctx, uint64_t faultAddr) noexcept {
    DecodedFault d{};
    if (!code || codeLen < 2) return d;

    size_t p = 0;
    bool rexX = false, rexB = false;
    while (p < codeLen) {
        uint8_t b = code[p];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
            ++p;
            continue;
        }
        break;
    }
    if (p >= codeLen) return d;

    if ((code[p] & 0xF0) == 0x40) {
        uint8_t rex = code[p++];
        rexX = (rex & 0x2) != 0;
        rexB = (rex & 0x1) != 0;
    }
    if (p >= codeLen) return d;

    uint8_t op0 = code[p++];
    if (op0 == 0x0F) {
        if (p >= codeLen) return d;
        uint8_t op1 = code[p++];
        if (op1 == 0x38 || op1 == 0x3A) {
            if (p >= codeLen) return d;
            ++p;
        }
    }
    if (p >= codeLen) return d;

    uint8_t modrm = code[p++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm  = modrm & 7;
    if (mod == 3) return d;

    int baseIdx = -1, indexIdx = -1;
    uint8_t scale = 1;
    bool ripRel = false;
    bool needDisp32 = false;

    if (rm == 4) {
        if (p >= codeLen) return d;
        uint8_t sib = code[p++];
        scale = static_cast<uint8_t>(1u << ((sib >> 6) & 3));
        uint8_t idx = (sib >> 3) & 7;
        uint8_t bse = sib & 7;
        if (!(idx == 4 && !rexX)) indexIdx = idx + (rexX ? 8 : 0);
        if (bse == 5 && mod == 0) { baseIdx = -1; needDisp32 = true; }
        else baseIdx = bse + (rexB ? 8 : 0);
    } else if (rm == 5 && mod == 0) {
        ripRel = true;
        needDisp32 = true;
    } else {
        baseIdx = rm + (rexB ? 8 : 0);
    }

    int64_t disp = 0;
    if (mod == 1) {
        if (p >= codeLen) return d;
        disp = static_cast<int8_t>(code[p++]);
    } else if (mod == 2 || needDisp32) {
        if (p + 4 > codeLen) return d;
        disp = ReadDisp32(code + p);
        p += 4;
    }

    d.ok          = true;
    d.ripRelative = ripRel;
    d.scale       = scale;
    d.disp        = disp;
    if (baseIdx >= 0) {
        d.baseHasReg = true;
        CopyReg(d.baseReg, kRegNames[baseIdx]);
        d.baseValue  = RegVal(ctx, baseIdx);
        d.baseIsNull = (d.baseValue == 0);
    }
    if (indexIdx >= 0) {
        d.indexHasReg = true;
        CopyReg(d.indexReg, kRegNames[indexIdx]);
    }
    BuildOperand(d);

    if (!ripRel) {
        uint64_t ea = (baseIdx >= 0 ? RegVal(ctx, baseIdx) : 0ull);
        if (indexIdx >= 0) ea += RegVal(ctx, indexIdx) * scale;
        ea += static_cast<uint64_t>(disp);
        d.computedEA = ea;
        d.eaMatches  = (ea == faultAddr);
    }
    return d;
}

}
