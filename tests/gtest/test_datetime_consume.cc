#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <print>
#include <string_view>

namespace {

constexpr std::size_t kIterations = 1'000'000;
constexpr int kRuns = 7;
constexpr char kTimeFormat[] = "%Y-%m-%d %H:%M:%S";

// 保存基准计算结果，防止编译器删除整个计时循环。
std::atomic<std::uint64_t> benchmark_sink{0};

std::uint64_t Observe(std::string_view value) noexcept {
  if (value.empty()) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.size()) +
         static_cast<unsigned char>(value.front()) +
         static_cast<unsigned char>(value[value.size() / 2]) +
         static_cast<unsigned char>(value.back());
}

// 将时间戳转换为本地时间字符串；返回实际写入的字符数。
std::size_t FormatLocalTime(std::time_t timestamp, char* output,
                            std::size_t capacity) noexcept {
  std::tm local_time{};
  if (localtime_r(&timestamp, &local_time) == nullptr) {
    return 0;
  }
  return std::strftime(output, capacity, kTimeFormat, &local_time);
}

std::time_t CurrentSecond() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::time_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::uint64_t FormatAndObserve(std::time_t timestamp) noexcept {
  std::array<char, 20> output{};
  const std::size_t size =
      FormatLocalTime(timestamp, output.data(), output.size());
  return Observe(std::string_view{output.data(), size});
}

// 只在秒数改变时调用 localtime_r/strftime，其余时间复用缓存结果。
class CachedDateTime {
 public:
  std::string_view Format(std::time_t current_second) noexcept {
    if (!initialized_ || current_second != last_second_) {
      size_ = FormatLocalTime(current_second, cached_.data(), cached_.size());
      last_second_ = current_second;
      initialized_ = true;
    }
    return {cached_.data(), size_};
  }

 private:
  std::array<char, 20> cached_{};
  std::size_t size_ = 0;
  std::time_t last_second_ = 0;
  bool initialized_ = false;
};

template <typename Operation>
std::chrono::nanoseconds Measure(std::size_t iterations,
                                 Operation&& operation) {
  std::uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < iterations; ++index) {
    checksum += operation();
  }
  const auto end = std::chrono::steady_clock::now();
  benchmark_sink.fetch_xor(checksum, std::memory_order_relaxed);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
}

template <typename Benchmark>
std::chrono::nanoseconds Average(Benchmark&& benchmark) {
  std::chrono::nanoseconds total{};
  for (int run = 0; run < kRuns; ++run) {
    total += benchmark();
  }
  return total / kRuns;
}

std::chrono::nanoseconds RemoveBaseline(std::chrono::nanoseconds measured,
                                         std::chrono::nanoseconds baseline) {
  return measured > baseline ? measured - baseline
                             : std::chrono::nanoseconds::zero();
}

TEST(DateTimeConsumeTest, CachedFormattingMatchesUncachedFormatting) {
  // 先验证缓存路径和标准转换路径对同一秒产生相同文本。
  const std::time_t current_second = CurrentSecond();
  std::array<char, 20> expected_buffer{};
  const std::size_t expected_size = FormatLocalTime(
      current_second, expected_buffer.data(), expected_buffer.size());
  ASSERT_NE(expected_size, 0U);

  CachedDateTime cached_date_time;
  const std::string_view expected{expected_buffer.data(), expected_size};
  const std::string_view first = cached_date_time.Format(current_second);
  const std::string_view second = cached_date_time.Format(current_second);
  EXPECT_EQ(first, expected);
  EXPECT_EQ(second, first);

  // 保留原测试的基准：空循环、每次转换、按秒缓存转换。
  const auto baseline = Average(
      [] { return Measure(kIterations, [] { return std::uint64_t{1}; }); });
  const auto uncached = Average([] {
    return Measure(kIterations,
                   [] { return FormatAndObserve(std::time(nullptr)); });
  });
  CachedDateTime benchmark_cache;
  const auto cached = Average([&] {
    return Measure(kIterations, [&] {
      return Observe(benchmark_cache.Format(CurrentSecond()));
    });
  });

  const auto uncached_adjusted = RemoveBaseline(uncached, baseline);
  const auto cached_adjusted = RemoveBaseline(cached, baseline);

  std::println("基础耗时: {} ns", baseline.count());
  std::println("原耗时: {} ns", uncached_adjusted.count());
  std::println("现耗时: {} ns", cached_adjusted.count());

  const double speedup =
      cached_adjusted.count() == 0
          ? 0.0
          : static_cast<double>(uncached_adjusted.count()) /
                static_cast<double>(cached_adjusted.count());
  std::println("优化倍数: {:.2f}", speedup);
}

}  // namespace
