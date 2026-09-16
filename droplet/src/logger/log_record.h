#pragma once

#include <droplet/types.h>

#include <droplet/logger/log_level.h>
#include <droplet/macros.h>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace droplet::detail {

struct LogRecordView {
  LogLevel::Level level_ = LogLevel::Level::LOG_LV_DEBUG;
  std::string_view loggerName_;                      // 日志器的名字
  std::string_view message_;                         // 日志内容
  std::chrono::system_clock::time_point timestamp_;  // 时间戳
  std::chrono::steady_clock::duration elapsed_;      // 起服已过时间
  u64 threadId_ = 0;                                 // 线程ID
  u64 fiberId_ = 0;                                  // 协程ID
  std::string_view threadName_;                      // 线程名
  std::string_view fileName_;                        // 文件名
  u32 line_ = 0;                           // 行号
};

}  // namespace droplet::detail
