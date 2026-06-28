#pragma once
#include <windows.h>
#include <chrono>

namespace redscope {
class PreallocatedBuffer;
struct Snapshot;
}

namespace redscope::rtti {
struct InFlightSet;
}

namespace redscope::report {

void BuildJsonSidecar(PreallocatedBuffer& out,
                      const EXCEPTION_POINTERS* ep,
                      const Snapshot* snapshot,
                      std::chrono::system_clock::time_point crashTime,
                      const rtti::InFlightSet* inFlight = nullptr) noexcept;

void BuildDiagnosticJson(const Snapshot* snapshot, PreallocatedBuffer& out) noexcept;

}
