#pragma once

#include <droplet/logger/appender.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string_view>

namespace droplet {
class Appender::Impl {
 public:
  virtual ~Impl() = default;
  void append(LogLevel::Level level,
              std::string_view formatted_record) noexcept;
  void setLevel(LogLevel::Level level) noexcept;
  [[nodiscard]] LogLevel::Level getLevel() const noexcept;
  void flush();
  void sync();

 protected:
  virtual void writeUnlocked(std::string_view formatted_record) noexcept = 0;
  virtual void flushUnlocked() noexcept = 0;
  virtual void syncUnlocked() noexcept = 0;

  std::mutex mutex_;

 private:
  // 全局level，小于他的日志就不管
  std::atomic<LogLevel::Level> level_{LogLevel::Level::LOG_LV_DEBUG};
};

namespace detail {

class AppenderAccess final {
 public:
  [[nodiscard]] static AppenderPtr MakeStdoutAppender();
  [[nodiscard]] static AppenderPtr MakeFileAppender(std::string file_name);
  static void Append(const AppenderPtr& appender, LogLevel::Level level,
                     std::string_view formatted_record) noexcept;
};

}  // namespace detail
}  // namespace droplet