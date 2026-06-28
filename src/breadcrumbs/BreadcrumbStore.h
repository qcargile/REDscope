#pragma once
#include "Ring.h"
#include "Breadcrumb.h"

namespace redscope {

using BreadcrumbRing = Ring<Breadcrumb, 256>;

struct BreadcrumbStore {
    BreadcrumbRing ring;
    int64_t        initTimeNs = 0;
};

BreadcrumbStore& GetBreadcrumbStore();
void             InitBreadcrumbStore();

void Crumb(uint32_t kind, const char* tag, const char* message) noexcept;

}
