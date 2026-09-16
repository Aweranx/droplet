#include "logger/formatter.h"
#include <droplet/types.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <unordered_map>

namespace droplet::detail {

constexpr std::string_view DEFAULT_DATE_FORMAT = "%Y-%m-%d %H:%M:%S";

Formatter::Formatter(std::string_view pattern) : pattern_(pattern) {
  if (parse() != 0) {
    throw std::invalid_argument("invalid logger format pattern");
  }
}

void Formatter::format(const LogRecordView& record,
                       FormattedRecordBuffer& output) const {
  for (const auto& item : items_) {
    item->format(record, output);
  }
}

// 把T变为int加入到output里
template <typename T>
void AppendInteger(FormattedRecordBuffer& output, T value) {
  std::array<char, 24> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                    value);  // 基本上不会失败，转失败了也没关系
  ASSERT_RETNONE2(result.ec == std::errc(),
                  std::format("trans {} to chars failed.", value));
  output.append(buffer.data(),
                static_cast<std::size_t>(result.ptr - buffer.data()));
}

// 几个类别的FormatItem
class LiteralFormatItem final : public Formatter::FormatItem {
 public:
  explicit LiteralFormatItem(std::string value) : value_(std::move(value)) {}
  void format(const LogRecordView&,
              FormattedRecordBuffer& output) const override {
    output.append(value_);
  }

 private:
  std::string value_;
};

class MessageFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    output.append(record.message_);
  }
};

class LevelFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    output.append(LogLevel::ToString(record.level_));
  }
};

class ElapsedFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    AppendInteger(output, std::chrono::duration_cast<std::chrono::milliseconds>(
                              record.elapsed_)
                              .count());
  }
};

class LoggerNameFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    output.append(record.loggerName_);
  }
};

class ThreadIdFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    AppendInteger(output, record.threadId_);
  }
};

class NewLineFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView&,
              FormattedRecordBuffer& output) const override {
    output.append('\n');
  }
};

// 优化：同一秒内产生大量日志时，不要反复进行时区转换和日期格式化。
// 从时间戳转化到本地时间的操作localtime_r开销很大
// 如果是同一秒的日志，直接复用cached_date_time
class DateTimeFormatItem final : public Formatter::FormatItem {
 public:
  explicit DateTimeFormatItem(std::string_view format)
      : format_(format.empty() ? DEFAULT_DATE_FORMAT : format) {}

  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    // Warning: 同一个进程中的datetime format要一致，否则会串
    static thread_local time_t last_second = 0;
    static thread_local char cached_date_time[20] = {'\0'};

    const auto duration = record.timestamp_.time_since_epoch();
    const time_t current_second = static_cast<time_t>(
        std::chrono::duration_cast<std::chrono::seconds>(duration).count());
    if (current_second != last_second) {
      std::tm buffer{};
#if defined(_WIN32)
      const errno_t result = localtime_s(&buffer, &current_second);
      ASSERT_RETNONE2(result == 0, "failed to convert log time to local time");
#else
      /**
       * @brief localtime_r是线程安全的，但是消耗也大：
       * 1. 全局有锁可能排队
       * 2. 需要进行复杂的时区计算(尤其是有过夏令时变更历史的时区)
       * 3. 如果系统没有加载时区信息或者要求响应时区变更,
       * 每次调用这个接口还要去发起磁盘IO，读系统文件/etc/localtime
       */
      // 把current_second转化为当前系统时区时间
      const std::tm* result = localtime_r(&current_second, &buffer);
      ASSERT_RETNONE2(result != nullptr,
                      "failed to convert log time to local time");
#endif
      // 把buffer里的时间按format_格式化写入到cached_date_time
      const std::size_t size = std::strftime(
          cached_date_time, sizeof(cached_date_time), format_.c_str(), &buffer);
      ASSERT_RETNONE2(size != 0, "failed to format log time");
      last_second = current_second;
    }

    output.append(cached_date_time);
  }

 private:
  std::string format_;
};

class MicrosecondsFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    const auto total_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            record.timestamp_.time_since_epoch())
            .count();
    i64 microseconds = total_microseconds % 1'000'000;
    if (microseconds < 0) {
      microseconds += 1'000'000;
    }

    // 秒内微秒必须补齐成固定六位，否则 12:00:00.1234
    // 这样的时间戳无法解析，也无法按字典序排序。
    std::array<char, MICROSECONDS_WIDTH> digits{};
    auto remaining = static_cast<u32>(microseconds);
    for (std::size_t index = digits.size(); index-- > 0;) {
      digits[index] = static_cast<char>('0' + remaining % 10);
      remaining /= 10;
    }
    output.append(digits.data(), digits.size());
  }

 private:
  static constexpr std::size_t MICROSECONDS_WIDTH = 6;
};

class FileNameFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    output.append(record.fileName_);
  }
};

class LineFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    AppendInteger(output, record.line_);
  }
};

class TabFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView&,
              FormattedRecordBuffer& output) const override {
    output.append('\t');
  }
};

class FiberIdFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    AppendInteger(output, record.fiberId_);
  }
};

class ThreadNameFormatItem final : public Formatter::FormatItem {
 public:
  void format(const LogRecordView& record,
              FormattedRecordBuffer& output) const override {
    output.append(record.threadName_);
  }
};

// 函数指针，用于指代下面的CreateSimpleFormatItem和CreateConfiguredFormatItem
using FormatItemFactory =
    std::unique_ptr<Formatter::FormatItem> (*)(std::string_view);

template <typename Item>
[[nodiscard]] std::unique_ptr<Formatter::FormatItem> CreateSimpleFormatItem(
    std::string_view) {
  return std::make_unique<Item>();
}

template <typename Item>
[[nodiscard]] std::unique_ptr<Formatter::FormatItem> CreateConfiguredFormatItem(
    std::string_view format) {
  return std::make_unique<Item>(format);
}

[[nodiscard]] const std::unordered_map<char, FormatItemFactory>&
GetFormatItemFactories() {
  static const std::unordered_map<char, FormatItemFactory>
      FORMAT_ITEM_FACTORIES{
          {'m', &CreateSimpleFormatItem<MessageFormatItem>},  // 日志内容
          {'p', &CreateSimpleFormatItem<LevelFormatItem>},    // LogLevel
          {'r', &CreateSimpleFormatItem<ElapsedFormatItem>},  // 起服已过时间
          {'c', &CreateSimpleFormatItem<LoggerNameFormatItem>},  // 日志器名称
          {'t', &CreateSimpleFormatItem<ThreadIdFormatItem>},      // 线程id
          {'n', &CreateSimpleFormatItem<NewLineFormatItem>},       // 换行
          {'d', &CreateConfiguredFormatItem<DateTimeFormatItem>},  // datetime
          {'u', &CreateSimpleFormatItem<MicrosecondsFormatItem>},  // 毫秒数
          {'f', &CreateSimpleFormatItem<FileNameFormatItem>},      // 文件名
          {'l', &CreateSimpleFormatItem<LineFormatItem>},          // 行号
          {'T', &CreateSimpleFormatItem<TabFormatItem>},           // 制表符
          {'F', &CreateSimpleFormatItem<FiberIdFormatItem>},       // 协程ID
          {'N', &CreateSimpleFormatItem<ThreadNameFormatItem>},  // 大N线程名
      };
  return FORMAT_ITEM_FACTORIES;
}

[[nodiscard]] std::unique_ptr<Formatter::FormatItem> CreateFormatItem(
    char directive, std::string_view format) {
  const auto& factories = GetFormatItemFactories();
  const auto it = factories.find(directive);
  ASSERT_RETVAL2(it != factories.end(), nullptr,
                 std::format("unknown logger format directive: {}", directive));
  return it->second(format);
}

void Formatter::addLiteral(std::string& literal) {
  if (literal.empty()) {
    return;
  }
  items_.emplace_back(std::make_unique<LiteralFormatItem>(std::move(literal)));
  literal.clear();
}

// %d{%Y-%m-%d %H:%M:%S}.%u%Tthread=%t%Tfiber=%F%T[%p]%T%f:%l%T%m%n
int Formatter::parse() {
  // 先把连续的普通字符收集起来，遇到格式指令时再一次性创建字面量格式项。
  // 这样既能保留 pattern 中的原文，也能避免为每个普通字符创建一个对象。
  std::string literal;
  for (std::size_t index = 0; index < pattern_.size(); ++index) {
    if (pattern_[index] != '%') {
      // 非 '%' 字符属于固定文本，暂存到当前字面量中。
      literal.push_back(pattern_[index]);
      continue;
    }

    // '%' 必须后跟一个指令字符，不能让 pattern 以孤立的 '%' 结束。
    ASSERT_RETVAL2(index + 1 < pattern_.size(), -1,
                   "logger format pattern ends with an incomplete directive");

    // 跳过 '%' 并读取指令字符，例如 %m 中的 'm'。
    const char directive = pattern_[++index];
    if (directive == '%') {
      // '%%' 是转义写法，输出一个普通的 '%'，而不是创建格式项。
      literal.push_back('%');
      continue;
    }

    // 当前指令即将由专用 FormatItem 处理，先把它之前积累的普通文本提交出去。
    addLiteral(literal);

    // 某些指令允许携带花括号参数，例如 %d{%Y-%m-%d %H:%M:%S}。
    // 没有花括号参数的指令保持空 string_view。
    std::string_view item_format;
    if (index + 1 < pattern_.size() && pattern_[index + 1] == '{') {
      // 从当前指令后面的 '{' 开始查找对应的 '}'。
      const std::size_t closing_brace = pattern_.find('}', index + 2);

      // 要求参数必须非空且存在结束花括号，避免产生无法工作的格式项。
      ASSERT_RETVAL2(
          closing_brace != std::string::npos && closing_brace != (index + 2),
          -2, "missing a closing brace or empty");

      // string_view 直接指向 pattern_ 的内容；Formatter 持有
      // pattern_，因而解析期间有效。
      item_format = std::string_view(pattern_).substr(
          index + 2, closing_brace - index - 2);

      // 跳过整个 '{...}' 参数，for 循环的自增会从参数结束位置继续解析。
      index = closing_brace;
    }

    // 根据指令字符查找对应的 FormatItem 工厂，并保存到解析后的项目列表。
    // 例如 'm' 创建 MessageFormatItem，'d' 创建 DateTimeFormatItem。
    items_.push_back(CreateFormatItem(directive, item_format));
  }

  // 循环结束后，pattern 末尾可能还剩一段普通文本，需要补充为最后一个字面量项。
  addLiteral(literal);

  // 工厂查找失败时会返回空指针；统一检查，避免 format() 时解引用空指针。
  for (const auto& item : items_) {
    ASSERT_RETVAL2(item != nullptr, -3,
                   "logger formatter contains a null format item");
  }

  // 返回 0 表示 pattern 已成功解析。
  return 0;
}

}  // namespace droplet::detail
