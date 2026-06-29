#pragma once
#include "Snapshot.h"
#include <cstdint>

namespace redscope::snap {

void Start(uint32_t intervalMs);
void Stop();

void FreezeForCrash() noexcept;

const Snapshot* Current() noexcept;

int64_t UptimeNsAtCrash() noexcept;

const wchar_t* GameRoot() noexcept;

}
