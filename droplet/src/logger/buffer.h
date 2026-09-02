#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <streambuf>
#include <string_view>
#include <vector>

namespace droplet::detail {

template <std::size_t INLINE_CAPACITY>
class InlineBuffer {
 public:
  void append(const char* data, std::size_t size) {
    if (size == 0) {
      return;
    }

    if (overflow_.empty() && size_ + size <= INLINE_CAPACITY) {
      std::memcpy(inline_.data() + size_, data, size);
      size_ += size;
      return;
    }

    if (overflow_.empty()) {
      const std::size_t required_capacity = size_ + size;
      overflow_.reserve(std::max(INLINE_CAPACITY * 2, required_capacity));
      overflow_.insert(overflow_.end(), inline_.data(), inline_.data() + size_);
    }
    overflow_.insert(overflow_.end(), data, data + size);
    size_ = overflow_.size();
  }

  void append(std::string_view value) { append(value.data(), value.size()); }

  void append(char value) { append(&value, 1); }

  [[nodiscard]] const char* data() const noexcept {
    return overflow_.empty() ? inline_.data() : overflow_.data();
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] std::string_view view() const noexcept {
    return {data(), size()};
  }

 private:
  std::array<char, INLINE_CAPACITY> inline_{};
  std::vector<char> overflow_;
  std::size_t size_ = 0;
};

// 流式输入接口
template <std::size_t INLINE_CAPACITY>
class SmallStreamBuffer final : public std::streambuf {
 public:
  explicit SmallStreamBuffer(InlineBuffer<INLINE_CAPACITY>& buffer) noexcept
      : buffer_(buffer) {}

 protected:
  std::streamsize xsputn(const char* data, std::streamsize size) override {
    if (size <= 0) {
      return 0;
    }

    buffer_.append(data, static_cast<std::size_t>(size));
    return size;
  }

  int_type overflow(int_type character) override {
    // overflow，除了可以接受一般字符，还可能会接收流终止符EOF
    // 如果我们遇到了EOF,
    // 把它转成非EOF值向上报告写入成功，实际上不向Buffer里写入任何值。
    if (traits_type::eq_int_type(character, traits_type::eof())) {
      return traits_type::not_eof(character);
    }

    buffer_.append(traits_type::to_char_type(character));
    return character;
  }

 private:
  InlineBuffer<INLINE_CAPACITY>& buffer_;
};

// template <std::size_t INLINE_CAPACITY>
// class InlineBufferOutputIterator {
//  public:
//   using difference_type = std::ptrdiff_t;

//   explicit InlineBufferOutputIterator(
//       droplet::detail::InlineBuffer<INLINE_CAPACITY>& buffer)
//       : buffer_(&buffer) {}

//   InlineBufferOutputIterator& operator*() noexcept { return *this; }

//   InlineBufferOutputIterator& operator++() noexcept { return *this; }

//   InlineBufferOutputIterator operator++(int) noexcept { return *this; }

//   InlineBufferOutputIterator& operator=(char value) noexcept {
//     buffer_->append(value);
//     return *this;
//   }

//  private:
//   droplet::detail::InlineBuffer<INLINE_CAPACITY>* buffer_;
// };

}  // namespace droplet::detail