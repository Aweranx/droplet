#include <droplet/config/convert.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace droplet::detail {

namespace {

[[nodiscard]] constexpr char ToLowerAscii(char ch) noexcept {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

constexpr std::array<std::string_view, 6> TRUE_WORDS{"true", "yes", "on",
                                                     "y",    "1",   "t"};
constexpr std::array<std::string_view, 6> FALSE_WORDS{"false", "no", "off",
                                                      "n",     "0",  "f"};

// 忽略大小写是否相等
[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs,
                                    std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ParseBool(std::string_view text, bool& out) noexcept {
  const std::string_view body = StringUtils::Trim(text);
  for (const std::string_view word : TRUE_WORDS) {
    if (EqualsIgnoreCase(body, word)) {
      out = true;
      return true;
    }
  }

  for (const std::string_view word : FALSE_WORDS) {
    if (EqualsIgnoreCase(body, word)) {
      out = false;
      return true;
    }
  }
  return false;
}

const std::string& ScalarText(const YAML::Node& node,
                              std::string_view type_name) {
  if (!node.IsDefined()) {
    ThrowConvertError(type_name, "节点不存在");
  }
  if (!node.IsScalar()) {
    ThrowConvertError(type_name, "期望 YAML 标量节点");
  }
  return node.Scalar();
}

}  // namespace droplet::detail