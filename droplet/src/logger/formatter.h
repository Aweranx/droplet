#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "logger/buffer.h"
#include "logger/buffer_config.h"
#include "logger/log_record.h"

namespace droplet::detail {

using FormattedRecordBuffer = InlineBuffer<FORMATTED_RECORD_INLINE_CAPACITY>;

// 设置 pattern
//     ↓
// Formatter::parse()
//     ↓
// 把 pattern 拆成按顺序排列的 FormatItem
//     ↓
// 收到一条 LogRecordView
//     ↓
// Formatter::format(record, output)
//     ↓
// 按 items_ 顺序逐个读取 record 中的字段
//     ↓
// 将结果追加到 FormattedRecordBuffer
//     ↓
// Appender 输出到终端或文件

class Formatter final {
 public:
  class FormatItem {
   public:
    virtual ~FormatItem() = default;
    virtual void format(const LogRecordView& record,
                        FormattedRecordBuffer& ouput) const = 0;
  };

  explicit Formatter(std::string_view pattern);
  void format(const LogRecordView& record, FormattedRecordBuffer& ouput) const;

  [[nodiscard]] const std::string& getPattern() const noexcept {
    return pattern_;
  }

 private:
  int parse();
  void addLiteral(std::string& literal);

 private:
  std::string pattern_;
  std::vector<std::unique_ptr<FormatItem>> items_;
};

}  // namespace droplet::detail