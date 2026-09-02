#pragma once

#include <droplet/export.h>
#include <droplet/logger/log_level.h>

#include <memory>
#include <string>

namespace droplet {

// 用来代理我们的内部权限，严格控制谁能访问我们的Appender私有实现
namespace detail {
class AppenderAccess;
}

class DROPLET_API Appender final {
 public:
  // 声明一个内部实现类, 类外定义
  class Impl;
  ~Appender() = default;

  Appender(const Appender&) = delete;
  Appender& operator=(const Appender&) = delete;
  Appender(Appender&&) = delete;
  Appender& operator=(Appender&&) = delete;

  void setLevel(LogLevel::Level level) noexcept;
  [[nodiscard]] LogLevel::Level getLevel() const noexcept;

  void flush();
  void sync();

 private:
  explicit Appender(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class detail::AppenderAccess;
};

using AppenderPtr = std::shared_ptr<Appender>;

// 对外提供创建appender的接口
// 通过AppenderAccess访问到Appender的构造函数
[[nodiscard]] DROPLET_API AppenderPtr MakeStdoutAppender();
[[nodiscard]] DROPLET_API AppenderPtr MakeFileAppender(std::string file_name);

}  // namespace droplet