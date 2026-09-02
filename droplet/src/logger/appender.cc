#include <droplet/macros.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "logger/appender_impl.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace droplet {

Appender::Appender(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

void Appender::setLevel(LogLevel::Level level) noexcept {
  impl_->setLevel(level);
}

LogLevel::Level Appender::getLevel() const noexcept {
  return impl_->getLevel();
}

void Appender::flush() { impl_->flush(); }

void Appender::sync() { impl_->sync(); }

void Appender::Impl::append(LogLevel::Level level,
                            std::string_view formatted_record) noexcept {
  // 小于设置的全局level就不管
  if (static_cast<u8>(level) < static_cast<u8>(getLevel())) {
    return;
  }

  // std::mutex::lock()函数，在极端情况下会抛出std::system_error异常,
  // lock_guard的构造函数也不是noexcept的
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    writeUnlocked(formatted_record);
    if (level == LogLevel::Level::LOG_LV_FATAL) {
      flushUnlocked();
    }
  } catch (...) {
    constexpr std::string_view message =
        "pans logger: appender operation failed\n";
    std::fwrite(message.data(), 1, message.size(), stderr);
  }
}

void Appender::Impl::setLevel(LogLevel::Level level) noexcept {
  level_.store(level, std::memory_order_release);
}

LogLevel::Level Appender::Impl::getLevel() const noexcept {
  return level_.load(std::memory_order_acquire);
}

// flush接口，把数据从程序的内存刷到操作系统缓冲区
void Appender::Impl::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  flushUnlocked();
}

// sync接口，把数据从操作系统的缓冲区刷到IO缓冲区
void Appender::Impl::sync() {
  std::lock_guard<std::mutex> lock(mutex_);
  syncUnlocked();
}

// 控制台打印appender
class StdoutAppenderImpl final : public Appender::Impl {
 protected:
  void writeUnlocked(std::string_view formatted_record) noexcept override {
    std::cout.write(formatted_record.data(),
                    static_cast<std::streamsize>(formatted_record.size()));
  }

  void flushUnlocked() noexcept override { std::cout.flush(); }

  void syncUnlocked() noexcept override { flushUnlocked(); }
};

// 文件日志
class FileAppenderImpl final : public Appender::Impl {
 public:
  explicit FileAppenderImpl(const std::string& file_name) {
    if (file_name.empty()) {
      throw std::invalid_argument("logger file name cannot be empty");
    }
    file_ = std::fopen(file_name.c_str(), "ab");
    if (file_ == nullptr) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to open logger file: " + file_name);
    }
  }

  ~FileAppenderImpl() override {
    if (file_ != nullptr) {
      std::fflush(file_);
      std::fclose(file_);
    }
  }

 protected:
  void writeUnlocked(std::string_view formatted_record) noexcept override {
    std::fwrite(formatted_record.data(), 1, formatted_record.size(), file_);
  }

  void flushUnlocked() noexcept override { std::fflush(file_); }

  void syncUnlocked() noexcept override {
    flushUnlocked();
#if defined(_WIN32)
    ::_commit(::_fileno(m_file));
#else
    ::fdatasync(::fileno(file_));
#endif
  }

 private:
  std::FILE* file_ = nullptr;
};

AppenderPtr detail::AppenderAccess::MakeStdoutAppender() {
  return AppenderPtr(new Appender(std::make_unique<StdoutAppenderImpl>()));
}

AppenderPtr detail::AppenderAccess::MakeFileAppender(std::string file_name) {
  return AppenderPtr(
      new Appender(std::make_unique<FileAppenderImpl>(file_name)));
}

void detail::AppenderAccess::Append(
    const AppenderPtr& appender, LogLevel::Level level,
    std::string_view formatted_record) noexcept {
  if (appender != nullptr) {
    appender->impl_->append(level, formatted_record);
  }
}

AppenderPtr MakeStdoutAppender() {
  return detail::AppenderAccess::MakeStdoutAppender();
}

AppenderPtr MakeFileAppender(std::string file_name) {
  return detail::AppenderAccess::MakeFileAppender(std::move(file_name));
}

}  // namespace droplet