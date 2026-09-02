#pragma once

#include <droplet/export.h>
#include <droplet/logger/appender.h>
#include <droplet/logger/log_level.h>
#include <droplet/utils/noncopyable.h>

#include <memory>
#include <string>
#include <string_view>

namespace droplet {

namespace detail {
class LoggerAccess;
}

class DROPLET_API Logger final : public Noncopyable {
 public:
  class Impl;
  explicit Logger(std::string name = "root");
  ~Logger();

  [[nodiscard]] bool shouldLog(LogLevel::Level level) const noexcept;
  void setLevel(LogLevel::Level level) noexcept;
  [[nodiscard]] LogLevel::Level getLevel() const noexcept;
  [[nodiscard]] std::string_view getName() const noexcept;

  void setFormatter(std::string_view pattern);
  [[nodiscard]] std::string getFormatterPattern() const;
  void addAppender(AppenderPtr appender);
  void removeAppender(const AppenderPtr& appender);
  void clearAppenders();

  void flush();
  void sync();

 private:
  std::unique_ptr<Impl> impl_;
  friend class detail::LoggerAccess;
};

using LoggerPtr = std::shared_ptr<Logger>;
[[nodiscard]] DROPLET_API LoggerPtr GetRootLogger();
[[nodiscard]] DROPLET_API LoggerPtr GetLogger(std::string_view name);

}  // namespace droplet