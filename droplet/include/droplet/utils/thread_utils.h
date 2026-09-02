#pragma once

#include <droplet/export.h>
#include <droplet/macros.h>

#include <string>
#include <string_view>

namespace droplet {
/**
 * @brief 获取线程ID
 *
 * 它在 Linux 下返回内核线程 ID，在Windows 下返回系统线程 ID；
 * 在其他平台返回 std::thread::id 的哈希值。
 */
[[nodiscard]] DROPLET_API u64 GetThreadId() noexcept;

/**
 * @brief 设置当前线程的名称。
 */
DROPLET_API void SetThreadName(std::string name);

/**
 * @brief 获取当前线程的名称
 */
[[nodiscard]] DROPLET_API std::string_view GetThreadName() noexcept;

}  // namespace droplet