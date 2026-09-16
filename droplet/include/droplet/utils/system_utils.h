#pragma once

#include <droplet/export.h>
#include <droplet/types.h>

#include <chrono>
#include <string>

namespace droplet {

[[nodiscard]] DROPLET_API std::chrono::steady_clock::duration
GetElapsedTime() noexcept;

[[nodiscard]] DROPLET_API u64 GetFiberId() noexcept;

[[nodiscard]] DROPLET_API std::string GetBacktrace(
    int size = 20, int skip = 1, const std::string& prefix = "");

}  // namespace droplet
