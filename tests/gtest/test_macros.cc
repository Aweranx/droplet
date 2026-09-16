#include <gtest/gtest.h>
#include <droplet/types.h>

#include <format>
#include <limits>
#include <print>
#include <type_traits>

#include <droplet/macros.h>

static_assert(sizeof(u8) == 1 && std::is_unsigned_v<u8>);
static_assert(sizeof(i8) == 1 && std::is_signed_v<i8>);
static_assert(sizeof(u16) == 2 && std::is_unsigned_v<u16>);
static_assert(sizeof(i16) == 2 && std::is_signed_v<i16>);
static_assert(sizeof(u32) == 4 && std::is_unsigned_v<u32>);
static_assert(sizeof(i32) == 4 && std::is_signed_v<i32>);
static_assert(sizeof(u64) == 8 && std::is_unsigned_v<u64>);
static_assert(sizeof(i64) == 8 && std::is_signed_v<i64>);
static_assert(INVALID8 == MAX_U8);
static_assert(INVALID16 == MAX_U16);
static_assert(INVALID32 == MAX_U32);
static_assert(INVALID64 == MAX_U64);
static_assert(MAX_U8 == std::numeric_limits<u8>::max());
static_assert(MAX_U16 == std::numeric_limits<u16>::max());
static_assert(MAX_U32 == std::numeric_limits<u32>::max());
static_assert(MAX_U64 == std::numeric_limits<u64>::max());

namespace {

// 用于验证 ASSERT_RETVAL/ASSERT_RETVAL2 的失败分支。
[[nodiscard]] int ReturnValueWithoutInfo() {
  ASSERT_RETVAL(false, -1);
  return 0;
}

[[nodiscard]] int ReturnValueWithInfo() {
  ASSERT_RETVAL2(false, -2, "return-value failure");
  return 0;
}

// 用于验证 ASSERT_RETNONE/ASSERT_RETNONE2 是否提前返回。
void ReturnNoneWithoutInfo(bool condition, bool& reached_end) {
  ASSERT_RETNONE(condition);
  reached_end = true;
}

void ReturnNoneWithInfo(bool condition, bool& reached_end) {
  ASSERT_RETNONE2(condition, "return-none failure");
  reached_end = true;
}

// 失败时应跳过当前循环迭代；Debug 下失败断言会先终止进程。
int RunContinue() {
  int visited = 0;
  for (int index = 0; index < 10; ++index) {
    ASSERT_CONTINUE(index != 5);
    ++visited;
  }
  return visited;
}

int RunContinueWithInfo() {
  int visited = 0;
  for (int index = 0; index < 10; ++index) {
    ASSERT_CONTINUE2(index != 5,
                     std::format("loop index should not be {}", index));
    ++visited;
  }
  return visited;
}

// 失败时应跳出循环；Debug 下失败断言会先终止进程。
int RunBreak() {
  int visited = 0;
  for (int index = 0; index < 10; ++index) {
    ASSERT_BREAK(index != 5);
    ++visited;
  }
  return visited;
}

int RunBreakWithInfo() {
  int visited = 0;
  for (int index = 0; index < 10; ++index) {
    ASSERT_BREAK2(index != 5,
                  std::format("loop index should not be {}", index));
    ++visited;
  }
  return visited;
}

// ASSERT_NOEFFECT 失败时不改变控制流；Debug 下仍会终止进程。
int RunNoEffect() {
  int visited = 0;
  for (int index = 0; index < 10; ++index) {
    ASSERT_NOEFFECT(index != 5);
    ++visited;
  }
  return visited;
}

int RunNoEffectWithInfo() {
  int visited = 0;
  for (int index = 0; index < 10; ++index) {
    ASSERT_NOEFFECT2(index != 5,
                     std::format("loop index should not be {}", index));
    ++visited;
  }
  return visited;
}

[[nodiscard]] int ReturnValueOnSuccess() {
  ASSERT_RETVAL(true, -1);
  return 42;
}

// 验证所有宏在条件为 true 时不会改变正常控制流。
TEST(MacrosTest, TypesAndConstants) {
  EXPECT_TRUE(std::is_unsigned_v<u8>);
  EXPECT_TRUE(std::is_signed_v<i8>);
  EXPECT_TRUE(std::is_unsigned_v<u16>);
  EXPECT_TRUE(std::is_signed_v<i16>);
  EXPECT_TRUE(std::is_unsigned_v<u32>);
  EXPECT_TRUE(std::is_signed_v<i32>);
  EXPECT_TRUE(std::is_unsigned_v<u64>);
  EXPECT_TRUE(std::is_signed_v<i64>);

  EXPECT_EQ(INVALID8, MAX_U8);
  EXPECT_EQ(INVALID16, MAX_U16);
  EXPECT_EQ(INVALID32, MAX_U32);
  EXPECT_EQ(INVALID64, MAX_U64);
  EXPECT_EQ(MAX_U8, std::numeric_limits<u8>::max());
  EXPECT_EQ(MAX_U16, std::numeric_limits<u16>::max());
  EXPECT_EQ(MAX_U32, std::numeric_limits<u32>::max());
  EXPECT_EQ(MAX_U64, std::numeric_limits<u64>::max());
}

TEST(MacrosTest, TrueConditionsPreserveControlFlow) {
  ASSERT_NOEFFECT(true);
  ASSERT_NOEFFECT2(true, "this condition is true");
  EXPECT_EQ(ReturnValueOnSuccess(), 42);

  int continued = 0;
  for (int index = 0; index < 3; ++index) {
    ASSERT_CONTINUE(index < 3);
    ++continued;
  }
  EXPECT_EQ(continued, 3);

  int broken = 0;
  for (int index = 0; index < 3; ++index) {
    ASSERT_BREAK(index < 3);
    ++broken;
  }
  EXPECT_EQ(broken, 3);
}

#if defined(DROPLET_DEBUG)

// Debug 模式下项目断言会终止进程，因此使用 GoogleTest death test 验证。
TEST(MacrosTest, FailedReturnMacrosTerminateInDebug) {
  EXPECT_DEATH((void)ReturnValueWithoutInfo(), "ASSERT FAILED");
  EXPECT_DEATH((void)ReturnValueWithInfo(), "failed");

  EXPECT_DEATH(
      {
        bool reached_end = false;
        ReturnNoneWithoutInfo(false, reached_end);
      },
      "ASSERT FAILED");
  EXPECT_DEATH(
      {
        bool reached_end = false;
        ReturnNoneWithInfo(false, reached_end);
      },
      "failed");
}

TEST(MacrosTest, FailedLoopMacrosTerminateInDebug) {
  EXPECT_DEATH(RunContinue(), "ASSERT FAILED");
  EXPECT_DEATH(RunContinueWithInfo(), "failed");
  EXPECT_DEATH(RunBreak(), "ASSERT FAILED");
  EXPECT_DEATH(RunBreakWithInfo(), "failed");
  EXPECT_DEATH(RunNoEffect(), "ASSERT FAILED");
  EXPECT_DEATH(RunNoEffectWithInfo(), "failed");
}

#else

// Release 模式下 assert 被禁用，验证各宏约定的返回值和控制流。
TEST(MacrosTest, FailedReturnMacrosReturnExpectedValuesInRelease) {
  EXPECT_EQ(ReturnValueWithoutInfo(), -1);
  EXPECT_EQ(ReturnValueWithInfo(), -2);

  bool reached_end = false;
  ReturnNoneWithoutInfo(false, reached_end);
  EXPECT_FALSE(reached_end);

  reached_end = false;
  ReturnNoneWithInfo(false, reached_end);
  EXPECT_FALSE(reached_end);
}

TEST(MacrosTest, FailedLoopMacrosPreserveReleaseControlFlow) {
  EXPECT_EQ(RunContinue(), 9);
  EXPECT_EQ(RunContinueWithInfo(), 9);
  EXPECT_EQ(RunBreak(), 5);
  EXPECT_EQ(RunBreakWithInfo(), 5);
  EXPECT_EQ(RunNoEffect(), 10);
  EXPECT_EQ(RunNoEffectWithInfo(), 10);
}

#endif

}  // namespace
