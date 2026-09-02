#pragma once

#include <droplet/export.h>
#include <droplet/macros.h>

#include <chrono>

namespace droplet {

[[nodiscard]] DROPLET_API std::chrono::steady_clock::duration
GetElapsedTime() noexcept;

[[nodiscard]] DROPLET_API u64 GetFiberId() noexcept;

}  // namespace droplet