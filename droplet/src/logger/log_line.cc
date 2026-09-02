#include <droplet/logger/log.h>
#include <droplet/utils/system_utils.h>
#include <droplet/utils/thread_utils.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <new>
#include <ostream>
#include <vector>

#include "logger/buffer.h"
#include "logger/buffer_config.h"
#include "logger/log_record.h"
#include "logger/logger_impl.h"

namespace droplet::detail {

struct LogLine::Impl final {
  Impl(Logger& logger, LogLevel::Level level, u32 line,
       std::string_view file_name)
      : m_logger(logger),
        m_level(level),
        m_line(line),
        m_fileName(file_name),
        m_timestamp(std::chrono::system_clock::now()),
        m_elapsed(GetElapsedTime()),
        m_threadId(GetThreadId()),
        m_fiberId(GetFiberId()),
        m_threadName(GetThreadName()),
        m_streamBuffer(m_inlineBuffer),
        m_stream(&m_streamBuffer) {}

  [[nodiscard]] LogRecordView getRecord() const noexcept {
    return {
        m_level,     m_logger.getName(), m_inlineBuffer.view(),
        m_timestamp, m_elapsed,          m_threadId,
        m_fiberId,   m_threadName,       m_fileName,
        m_line,
    };
  }

  Logger& m_logger;
  LogLevel::Level m_level;
  u32 m_line = 0;
  std::string_view m_fileName;
  std::chrono::system_clock::time_point m_timestamp;
  std::chrono::steady_clock::duration m_elapsed;
  u64 m_threadId = 0;
  u64 m_fiberId = 0;
  std::string_view m_threadName;
  InlineBuffer<LOG_MESSAGE_INLINE_CAPACITY> m_inlineBuffer;
  SmallStreamBuffer<LOG_MESSAGE_INLINE_CAPACITY> m_streamBuffer;
  std::ostream m_stream;
};

LogLine::LogLine(Logger& logger, LogLevel::Level level, u32 line,
                 std::string_view file_name) {
  static_assert(sizeof(Impl) <= LOG_LINE_IMPL_SIZE,
                "LogLine inline implementation storage is too small");
  static_assert(alignof(Impl) <= alignof(std::max_align_t),
                "LogLine implementation requires excessive alignment");
  std::construct_at(reinterpret_cast<Impl*>(implStorage_), logger, level, line,
                    file_name);
}

LogLine::Impl& LogLine::getImpl() noexcept {
  // launder明确获取当前存储区中新对象的有效指针
  return *std::launder(reinterpret_cast<Impl*>(implStorage_));
}

LogLine::~LogLine() noexcept {
  Impl& impl = getImpl();
  LoggerAccess::Submit(impl.m_logger, impl.getRecord());
  std::destroy_at(&impl);
}

std::ostream& LogLine::stream() noexcept { return getImpl().m_stream; }

void LogPrintf(Logger& logger, LogLevel::Level level, u32 line,
               std::string_view file_name, const char* format, ...) {
  LogLine log_line(logger, level, line, file_name);
  if (format == nullptr) {
    log_line.stream() << "<null-format>";
    return;
  }

  std::array<char, PRINTF_FORMAT_INLINE_CAPACITY> inline_buffer;

  // format = "id=%d, name=%s"
  // 可变参数 = 10, "droplet"
  va_list arguments;
  // 访问可变参数会破坏参数指针偏移量，为了能再次访问可变参数包，必须拷贝一份
  va_list arguments_copy;
  // 初始化参数游标
  // 执行后，arguments 就指向 format 后面的第一个可变参数：
  // arguments → 10 → "droplet"
  va_start(arguments, format);
  va_copy(arguments_copy, arguments);
  // vsnprintf 根据 format 读取 arguments，填入inline_buffer
  // 参数游标不断向后移动，结束后arguments被消耗
  const int required_size = std::vsnprintf(
      inline_buffer.data(), inline_buffer.size(), format, arguments);
  // 释放或清理参数游标占用的资源
  va_end(arguments);

  if (required_size < 0) {
    va_end(arguments_copy);
    log_line.stream() << "<format-error>";
    return;
  }

  const std::size_t message_size = static_cast<std::size_t>(required_size);
  if (message_size < inline_buffer.size()) {
    va_end(arguments_copy);
    log_line.stream().write(inline_buffer.data(),
                            static_cast<std::streamsize>(message_size));
    return;
  }

  // 如果日志长度过大，就在堆上分配
  // vsnprintf 会消耗 va_list，不能直接再次使用原来的 arguments
  // 所以要arguments_copy
  std::vector<char> overflow_buffer(message_size + 1);
  const int second_result = std::vsnprintf(
      overflow_buffer.data(), overflow_buffer.size(), format, arguments_copy);
  va_end(arguments_copy);
  if (second_result < 0) {
    log_line.stream() << "<format-error>";
    return;
  }

  log_line.stream().write(overflow_buffer.data(),
                          static_cast<std::streamsize>(message_size));
  // 到这里还只是在内存里格式化这条日志,
  // 出了这个函数，log_line对象析构，才把日志从内存提交到logger的appender中去
}

}  // namespace droplet::detail