#include <droplet/type_name.h>
#include <droplet/types.h>
#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct AnyStruct {};

enum class AnyEnum : unsigned int {
  Debug,
  Release,
};

template <typename T, int Size>
struct FixedArray {
  T values[Size];
};

template <typename T>
void PrintType(std::string_view label) {
  constexpr std::string_view type_name = droplet::TypeName<T>();
  static_assert(!type_name.empty());
  std::cout << label << " -> " << type_name << '\n';
}

}  // namespace

TEST(TestTypeName, compare) {
  EXPECT_EQ(droplet::TypeName<int>(), "int");
  EXPECT_EQ(droplet::TypeName<double>(), "double");
  EXPECT_EQ(droplet::TypeName<std::vector<int>>(), "std::vector<int>");
  EXPECT_EQ(droplet::TypeName<u8>(), "unsigned char");

  using Counter = int;
  EXPECT_EQ(droplet::TypeName<Counter>(), droplet::TypeName<int>());

  constexpr std::string_view first = droplet::TypeName<AnyStruct>();
  constexpr std::string_view second = droplet::TypeName<AnyStruct>();
  EXPECT_EQ(first, second);

  EXPECT_EQ(first.data(), second.data());

  EXPECT_NE(droplet::TypeName<const int&>(), droplet::TypeName<int>());
  EXPECT_NE(droplet::TypeName<int*>(), droplet::TypeName<int>());
  EXPECT_NE(droplet::TypeName<int[3]>(), droplet::TypeName<int>());

  PrintType<int>("int");
  PrintType<const int>("const int");
  PrintType<const int&>("const int&");
  PrintType<int*>("int pointer");
  PrintType<int[3]>("int array");
  PrintType<std::string>("std::string");
  PrintType<std::vector<int>>("std::vector<int>");
  PrintType<AnyStruct>("user-defined type");
  PrintType<AnyEnum>("enum");
  PrintType<FixedArray<long, 4>>("class template");
  PrintType<int (*)(double)>("function pointer");

  const auto lambda = [](int value) { return value + 1; };
  PrintType<decltype(lambda)>("lambda");

  std::cout << "alias Counter - >" << droplet::TypeName<Counter>()
            << " (会被还原成本来类型)" << std::endl;
}