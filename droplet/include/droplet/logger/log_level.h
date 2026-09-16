#pragma once

#include <droplet/types.h>

#include <droplet/export.h>

#include <cstdint>
#include <string_view>

namespace droplet {
class DROPLET_API LogLevel final {
 public:
  enum class Level : u8 {
    LOG_LV_DEBUG = 1,  // 记录调试细节，如变量值，程序执行流程
    LOG_LV_INFO = 2,  // 记录正常运行信息，比如服务启动，玩家连接成功
    LOG_LV_WARN = 3,  // 可能存在问题，但是程序还能继续执行
    LOG_LV_ERROR = 4,  // 操作失败，或者功能异常，需要检查和处理
    LOG_LV_FATAL = 5,  // 存在严重故障，程序继续执行，可能会有重大损失
    LOG_LV_OFF = 6,  // 用来关闭日志
  };

  [[nodiscard]] static std::string_view ToString(Level level) noexcept;
  [[nodiscard]] static LogLevel::Level FromString(
      std::string_view value) noexcept;
};
}  // namespace droplet
