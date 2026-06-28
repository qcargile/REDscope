#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace redscope::rtti {

struct DecodedFault {
    bool     ok          = false;
    bool     eaMatches   = false;
    bool     ripRelative = false;
    bool     baseHasReg  = false;
    bool     indexHasReg = false;
    bool     baseIsNull  = false;
    uint8_t  scale       = 1;
    int64_t  disp        = 0;
    uint64_t baseValue   = 0;
    uint64_t computedEA  = 0;
    char     baseReg[4]  = {};
    char     indexReg[4] = {};
    char     operand[48] = {};
};

DecodedFault DecodeFaultOperand(const uint8_t* code, size_t codeLen,
                                const CONTEXT& ctx, uint64_t faultAddr) noexcept;

}
