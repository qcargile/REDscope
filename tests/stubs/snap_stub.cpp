#include "../../src/snapshot/SnapshotWorker.h"
#include "../../src/snapshot/Snapshot.h"

namespace redscope::snap {

static const Snapshot* g_testSnapshot = nullptr;

const Snapshot* Current() noexcept { return g_testSnapshot; }
int64_t UptimeNsAtCrash() noexcept { return 0; }
const wchar_t* GameRoot() noexcept { return L""; }

void TestSetCurrentSnapshot(const Snapshot* s) noexcept { g_testSnapshot = s; }

}
