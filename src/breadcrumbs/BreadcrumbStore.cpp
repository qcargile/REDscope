#include "BreadcrumbStore.h"
#include "../util/Time.h"
#include <cstdint>

namespace redscope {
namespace {

BreadcrumbStore g_store;

}

BreadcrumbStore& GetBreadcrumbStore() { return g_store; }

void InitBreadcrumbStore() {
    if (g_store.initTimeNs == 0) g_store.initTimeNs = NowNs();
}

void Crumb(uint32_t kind, const char* tag, const char* message) noexcept {
    Breadcrumb b{};
    b.timestampNs = NowNs();
    b.kind        = kind;
    CopyFixed(b.tag,     kBreadcrumbTagLen, tag);
    CopyFixed(b.message, kBreadcrumbMsgLen, message);
    g_store.ring.Push(b);
}

}

extern "C" __declspec(dllexport)
void REDscope_Crumb(uint32_t kind, const char* tag, const char* message) noexcept {
    redscope::Crumb(kind, tag, message);
}
