#pragma once
#include "../util/FixedStr.h"
#include <cstdint>
#include <cstring>

namespace redscope {

constexpr size_t kBreadcrumbTagLen = 16;
constexpr size_t kBreadcrumbMsgLen = 116;

struct Breadcrumb {
    int64_t timestampNs;
    uint32_t kind;
    char tag[kBreadcrumbTagLen];
    char message[kBreadcrumbMsgLen];
};

static_assert(sizeof(Breadcrumb) == 8 + 4 + kBreadcrumbTagLen + kBreadcrumbMsgLen,
              "Breadcrumb size drifted - check field layout");

enum BreadcrumbKind : uint32_t {
    BcUser           = 0,
    BcDelayRegister  = 1,
    BcDelayFire      = 2,
    BcFindEntityNull = 3,
    BcTweakDbLookup  = 4,
    BcArchiveLoad    = 5,
    BcStatusEffect   = 6,
    BcStaticSpawn    = 7,
    BcStaticDespawn  = 8,
};

}
