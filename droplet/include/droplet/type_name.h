#pragma once

#include <cstddef>
#include <source_location>
#include <string_view>

namespace droplet::detail {

// 获取当前函数的完整签名
// TypeNameProbe函数在编译期实例化出签名以后被source_location引用
// 如果该签名在运行时被输出，就会被存储在elf文件的.rodata区，可以检索到
// strings ./bin/tests/test_type_name | grep TypeNameProbe
template <typename T>
[[nodiscard]] consteval std::string_view TypeNameProbe() noexcept {
#if defined(_MSC_VER)
  return __FUNCSIG__;
#else
  return std::source_location::current().function_name();
#endif
}

// MSVC: droplet::detail::TypeNameProbe<int>(void) noexcept
[[nodiscard]] constexpr std::string_view ExtractMsvcTypeName(
    std::string_view signature) noexcept {
  constexpr std::string_view marker = "TypeNameProbe<";
  constexpr std::string_view suffix = ">(void)";

  const std::size_t marker_pos = signature.rfind(marker);
  if (marker_pos == std::string_view::npos) {
    return signature;
  }

  const std::size_t begin = marker_pos + marker.size();
  const std::size_t end = signature.rfind(suffix);
  if (end == std::string_view::npos || end <= begin) {
    return signature;
  }
  return signature.substr(begin, end - begin);
}

// GCC: consteval std::string_view droplet::detail::TypeNameProbe() [with T =
// int;...] Clang: std::string_view droplet::detail::TypeNameProbe() [T = int]
// MSVC: droplet::detail::TypeNameProbe<int>(void) noexcept
template <typename T>
[[nodiscard]] consteval std::string_view ExtractTypeName() noexcept {
  constexpr std::string_view signature = TypeNameProbe<T>();

#if defined(_MSC_VER) && !defined(__clang__)
  constexpr std::string_view type_name = ExtractMsvcTypeName(signature);
  static_assert(type_name != signature, "无法识别当前 MSVC 的函数签名格式");
  return type_name;
#else
  constexpr std::string_view marker = "T = ";
  constexpr std::size_t marker_pos = signature.find(marker);

  if constexpr (marker_pos == std::string_view::npos) {
    return signature;
  } else {
    constexpr std::size_t begin = marker_pos + marker.size();
    constexpr std::size_t semicolon = signature.find(';', begin);
    constexpr std::size_t bracket = signature.find(']');
    constexpr std::size_t end =
        semicolon != std::string_view::npos ? semicolon : bracket;
    if constexpr (end == std::string_view::npos || end <= begin) {
      return signature.substr(begin);
    } else {
      return signature.substr(begin, end - begin);
    }
  }
#endif
}

template <typename T>
inline constexpr std::string_view TYPE_NAME = ExtractTypeName<T>();

}  // namespace droplet::detail

namespace droplet {

template <typename T>
[[nodiscard]] consteval std::string_view TypeName() noexcept {
  return detail::TYPE_NAME<T>;
}

}  // namespace droplet