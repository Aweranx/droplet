#pragma once

#include <droplet/types.h>

#include <droplet/logger/appender.h>
#include <droplet/logger/log_level.h>
#include <droplet/logger/logger.h>

#include <cstddef>
#include <iosfwd>
#include <string_view>

#include "droplet/export.h"

namespace droplet::detail {

class DROPLET_API LogLine final {
 public:
  LogLine(Logger& logger, LogLevel::Level level, u32 line,
          std::string_view file_name);
  ~LogLine() noexcept;

  LogLine(const LogLine&) = delete;
  LogLine& operator=(const LogLine&) = delete;
  LogLine(LogLine&&) = delete;
  LogLine& operator=(LogLine&&) = delete;

  [[nodiscard]] std::ostream& stream() noexcept;

 private:
  struct Impl;
  static constexpr std::size_t LOG_LINE_IMPL_SIZE = 1024;
  alignas(std::max_align_t) std::byte implStorage_[LOG_LINE_IMPL_SIZE];

  [[nodiscard]] Impl& getImpl() noexcept;
};

DROPLET_API void LogPrintf(Logger& logger, LogLevel::Level level, u32 line,
                           std::string_view file_name, const char* format, ...);

}  // namespace droplet::detail

// auto droplet_log_logger = (logger)
// 如果(logger)是一个表达式，那么在这个宏展开里只进行一次求值
#define DROPLET_LOG_LEVEL(logger, level)                                       \
  if (auto droplet_log_logger = (logger); !droplet_log_logger) {               \
  } else if (const auto droplet_log_level = (level);                           \
             !droplet_log_logger->shouldLog(droplet_log_level)) {              \
  } else                                                                       \
    droplet::detail::LogLine(*droplet_log_logger, droplet_log_level, __LINE__, \
                             __FILE__)                                         \
        .stream()

#define DROPLET_LOG_DEBUG(logger) \
  DROPLET_LOG_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_DEBUG)
#define DROPLET_LOG_INFO(logger) \
  DROPLET_LOG_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_INFO)
#define DROPLET_LOG_WARN(logger) \
  DROPLET_LOG_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_WARN)
#define DROPLET_LOG_ERROR(logger) \
  DROPLET_LOG_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_ERROR)
#define DROPLET_LOG_FATAL(logger) \
  DROPLET_LOG_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_FATAL)

#define DROPLET_LOG_FMT_LEVEL(logger, level, format, ...)              \
  if (auto droplet_log_logger = (logger); !droplet_log_logger) {       \
  } else if (const auto droplet_log_level = (level);                   \
             !droplet_log_logger->shouldLog(droplet_log_level)) {      \
  } else                                                               \
    droplet::detail::LogPrintf(*droplet_log_logger, droplet_log_level, \
                               __LINE__, __FILE__,                     \
                               (format)__VA_OPT__(, ) __VA_ARGS__)

#define DROPLET_LOG_FMT_DEBUG(logger, format, ...)                        \
  DROPLET_LOG_FMT_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_DEBUG, \
                        (format)__VA_OPT__(, ) __VA_ARGS__)
#define DROPLET_LOG_FMT_INFO(logger, format, ...)                        \
  DROPLET_LOG_FMT_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_INFO, \
                        (format)__VA_OPT__(, ) __VA_ARGS__)
#define DROPLET_LOG_FMT_WARN(logger, format, ...)                        \
  DROPLET_LOG_FMT_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_WARN, \
                        (format)__VA_OPT__(, ) __VA_ARGS__)
#define DROPLET_LOG_FMT_ERROR(logger, format, ...)                        \
  DROPLET_LOG_FMT_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_ERROR, \
                        (format)__VA_OPT__(, ) __VA_ARGS__)
#define DROPLET_LOG_FMT_FATAL(logger, format, ...)                        \
  DROPLET_LOG_FMT_LEVEL((logger), droplet::LogLevel::Level::LOG_LV_FATAL, \
                        (format)__VA_OPT__(, ) __VA_ARGS__)

#define DROPLET_LOG_ROOT() droplet::GetRootLogger()
#define DROPLET_LOG_NAME(name) droplet::GetLogger((name))

#define LOG_DEBUG DROPLET_LOG_DEBUG(g_logger)
#define LOG_INFO DROPLET_LOG_INFO(g_logger)
#define LOG_WARN DROPLET_LOG_WARN(g_logger)
#define LOG_ERROR DROPLET_LOG_ERROR(g_logger)
#define LOG_FATAL DROPLET_LOG_FATAL(g_logger)
