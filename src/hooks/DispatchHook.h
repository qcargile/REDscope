#pragma once

namespace redscope::hooks::dispatch {

void Install();
void Uninstall();

void ResolveStackNames() noexcept;
void ResolveHeartbeatNames() noexcept;

}
