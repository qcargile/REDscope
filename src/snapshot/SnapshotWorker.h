#pragma once
#include "Snapshot.h"
#include <cstdint>

namespace redscope::snap {

void Start(uint32_t intervalMs);
void Stop();

void FreezeForCrash() noexcept;

const Snapshot* Current() noexcept;

const wchar_t* GameRoot() noexcept;

}
