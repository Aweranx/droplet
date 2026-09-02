#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>

#include "logger/buffer.h"
#include "logger/buffer_config.h"

namespace {

constexpr int kRuns = 7;
constexpr std::size_t kDefaultOperations = 1'000'000;
constexpr std::array<std::size_t, 6> kMessageSizes = {
    50, 100, 200, 500, 1000, 2048};

// 防止编译器把基准循环中的结果优化掉。
std::atomic<std::uint64_t> benchmark_sink{0};

// 测试使用日志消息相同的内联容量，消息超过容量时会自动切换到堆内存。
using Buffer =
    droplet::detail::InlineBuffer<droplet::detail::LOG_MESSAGE_INLINE_CAPACITY>;

// 生成轻量校验值，既能比较写入结果，也能防止基准循环被优化掉。
std::uint64_t Observe(std::string_view value) noexcept {
  if (value.empty()) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.size()) +
         static_cast<unsigned char>(value.front()) +
         static_cast<unsigned char>(value[value.size() / 2]) +
         static_cast<unsigned char>(value.back());
}

std::uint64_t WriteWithSmallStreamBuffer(std::string_view message) {
  Buffer buffer;
  droplet::detail::SmallStreamBuffer<
      droplet::detail::LOG_MESSAGE_INLINE_CAPACITY>
      stream_buffer(buffer);
  std::ostream stream(&stream_buffer);
  stream << message;
  return Observe(buffer.view());
}

// 使用标准字符串流作为对照组，比较两种写入方式的结果和性能。
std::uint64_t WriteWithStringStream(std::string_view message) {
  std::stringstream stream;
  stream << message;
  return Observe(stream.view());
}

template <typename Operation>
// 执行指定次数并返回总耗时；checksum 会写入全局变量，避免循环被消除。
std::chrono::nanoseconds BenchmarkWrites(std::size_t operations,
                                          Operation&& operation) {
  std::uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < operations; ++iteration) {
    checksum += operation();
  }
  const auto end = std::chrono::steady_clock::now();
  benchmark_sink.fetch_xor(checksum, std::memory_order_relaxed);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
}

template <typename Benchmark>
// 对同一基准重复 kRuns 次，降低单次调度抖动对结果的影响。
double RunBenchmark(Benchmark&& benchmark) {
  long double total_nanoseconds = 0.0L;
  for (int run = 0; run < kRuns; ++run) {
    total_nanoseconds += static_cast<long double>(benchmark().count());
  }
  return static_cast<double>(total_nanoseconds / kRuns);
}

std::size_t BenchmarkOperations() {
  // GoogleTest 不接收自定义 operations 参数，因此通过环境变量配置。
  const char* value = std::getenv("DROPLET_BENCHMARK_OPERATIONS");
  if (value == nullptr || *value == '\0') {
    return kDefaultOperations;
  }

  try {
    const auto operations = std::stoull(value);
    return operations == 0 ? kDefaultOperations
                            : static_cast<std::size_t>(operations);
  } catch (...) {
    return kDefaultOperations;
  }
}

void PrintResult(std::string_view name, double total_nanoseconds,
                 std::size_t operations) {
  std::print("{:<30}{:.2f} ns total, {:.2f} ns/op\n", name,
             total_nanoseconds,
             total_nanoseconds / static_cast<double>(operations));
}

// 参数化测试夹具：同一组断言会针对多个消息长度自动执行。
class BufferComparisonTest
    : public ::testing::TestWithParam<std::size_t> {};

// TEST_P 表示参数化测试；GetParam() 返回当前测试实例的消息长度。
TEST_P(BufferComparisonTest, BothWritersPreserveTheMessage) {
  const std::string message(GetParam(), 'x');

  Buffer buffer;
  droplet::detail::SmallStreamBuffer<
      droplet::detail::LOG_MESSAGE_INLINE_CAPACITY>
      stream_buffer(buffer);
  std::ostream small_stream(&stream_buffer);
  small_stream << message;

  std::stringstream string_stream;
  string_stream << message;

  // ASSERT 失败时立即结束当前测试，EXPECT 失败后仍继续执行后续断言。
  ASSERT_TRUE(small_stream.good());
  ASSERT_TRUE(string_stream.good());
  // 两种流都必须完整保留原始消息。
  EXPECT_EQ(buffer.view(), std::string_view{message});
  EXPECT_EQ(string_stream.view(), std::string_view{message});
  // 校验两种实现产生的有效数据一致。
  EXPECT_EQ(Observe(buffer.view()), Observe(string_stream.view()));
}

// 为 TEST_P 提供 6 个参数，因此会生成 6 个独立的 CTest 用例。
INSTANTIATE_TEST_SUITE_P(MessageSizes, BufferComparisonTest,
                         ::testing::ValuesIn(kMessageSizes));

// TEST 是普通 GoogleTest 用例；这里保留 test_log_buffer.cc 的性能对比输出。
TEST(BufferBenchmarkTest, ComparesSmallStreamBufferWithStringStream) {
  const std::size_t operations = BenchmarkOperations();
  std::print("operations: {}, average of {} runs\n\n", operations, kRuns);

  for (const std::size_t message_size : kMessageSizes) {
    const std::string message(message_size, 'x');

    const auto small_stream_buffer_nanoseconds = RunBenchmark([&] {
      return BenchmarkWrites(operations, [&] {
        return WriteWithSmallStreamBuffer(message);
      });
    });
    const auto stringstream_nanoseconds = RunBenchmark([&] {
      return BenchmarkWrites(operations,
                             [&] { return WriteWithStringStream(message); });
    });

    // 先验证基准测试比较的是相同结果，再报告性能数据。
    ASSERT_EQ(WriteWithSmallStreamBuffer(message),
              WriteWithStringStream(message));

    std::print("message: {} characters\n", message_size);
    PrintResult("SmallStreamBuffer", small_stream_buffer_nanoseconds,
                operations);
    PrintResult("std::stringstream", stringstream_nanoseconds, operations);
    std::print("speedup: {:.2f}x\n\n",
               stringstream_nanoseconds / small_stream_buffer_nanoseconds);
  }
}

}  // namespace
