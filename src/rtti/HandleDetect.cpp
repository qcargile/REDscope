#include "HandleDetect.h"

namespace redscope::rtti {

bool LooksLikeRefBlock(uint32_t strongRefs, uint32_t weakRefs) noexcept {
    if (strongRefs == 0)                     return false;
    if (strongRefs >= kMaxPlausibleRefs)     return false;
    if (weakRefs >= kMaxPlausibleRefs)       return false;
    if (weakRefs < strongRefs)               return false;
    return true;
}

}
