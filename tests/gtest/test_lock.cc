#include <gtest/gtest.h>
#include <droplet/types.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <mutex>
#include <print>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kRuns = 7;
constexpr u64 kOperations = 1'000'000;
constexpr std::array<std::size_t, 5> kThreadCounts = {1, 2, 4, 8, 10};

u64 counter = 0;
std::atomic<u64> atomic_counter{0};
std::mutex mutex;
std::shared_mutex shared_mutex;
std::atomic<u64> benchmark_sink{0};

u64 OperationsForThread(std::size_t thread_index,
                                  std::size_t thread_count) {
  const u64 base = kOperations / thread_count;
  const u64 remainder = kOperations % thread_count;
  return base + (thread_index < remainder ? 1 : 0);
}

template <typename Operation>
std::chrono::nanoseconds BenchmarkThreads(std::size_t thread_count,
                                           Operation&& operation) {
  std::latch ready(thread_count);
  std::latch start_gate(1);
  std::latch finished(thread_count);

  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    threads.emplace_back([&, index] {
      ready.count_down();
      start_gate.wait();
      operation(index, OperationsForThread(index, thread_count));
      finished.count_down();
    });
  }

  ready.wait();
  const auto begin = std::chrono::steady_clock::now();
  start_gate.count_down();
  finished.wait();
  const auto end = std::chrono::steady_clock::now();

  for (std::thread& thread : threads) {
    thread.join();
  }

  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
}

std::chrono::nanoseconds TestMutex(std::size_t thread_count) {
  counter = 0;
  return BenchmarkThreads(
      thread_count, [](std::size_t /*thread_index*/, u64 operations) {
        for (u64 index = 0; index < operations; ++index) {
          std::lock_guard<std::mutex> lock(mutex);
          ++counter;
        }
      });
}

std::chrono::nanoseconds TestSharedMutex(std::size_t thread_count) {
  counter = 0;
  return BenchmarkThreads(
      thread_count, [](std::size_t /*thread_index*/, u64 operations) {
        for (u64 index = 0; index < operations; ++index) {
          std::unique_lock<std::shared_mutex> lock(shared_mutex);
          ++counter;
        }
      });
}

std::chrono::nanoseconds TestAtomic(std::size_t thread_count) {
  atomic_counter.store(0, std::memory_order_relaxed);
  return BenchmarkThreads(
      thread_count, [](std::size_t /*thread_index*/, u64 operations) {
        for (u64 index = 0; index < operations; ++index) {
          atomic_counter.fetch_add(1, std::memory_order_relaxed);
        }
      });
}

std::chrono::nanoseconds TestAtomicCAS(std::size_t thread_count) {
  atomic_counter.store(0, std::memory_order_relaxed);
  return BenchmarkThreads(
      thread_count, [](std::size_t /*thread_index*/, u64 operations) {
        for (u64 index = 0; index < operations; ++index) {
          u64 expected =
              atomic_counter.load(std::memory_order_relaxed);
          while (!atomic_counter.compare_exchange_weak(
              expected, expected + 1, std::memory_order_relaxed,
              std::memory_order_relaxed)) {
          }
        }
      });
}

template <typename Benchmark>
double RunBenchmark(Benchmark&& benchmark) {
  long double total_nanoseconds = 0.0L;
  for (int run = 0; run < kRuns; ++run) {
    total_nanoseconds += static_cast<long double>(benchmark().count());
  }
  return static_cast<double>(total_nanoseconds / kRuns);
}

void PrintResult(std::string_view name, double total_nanoseconds) {
  std::println("{:<14}: {:.2f} ns , {:.2f} ns/op", name,
               total_nanoseconds,
               total_nanoseconds / static_cast<double>(kOperations));
}

// 线程数是这个 benchmark 的输入维度，因此使用 TEST_P 自动生成多组测试。
class LockBenchmarkTest
    : public ::testing::TestWithParam<std::size_t> {};

TEST_P(LockBenchmarkTest, ComparesLockImplementations) {
  const std::size_t thread_count = GetParam();
  std::print("operations: {}, average of {} runs\n", kOperations, kRuns);
  std::println("{} threads", thread_count);

  const double mutex_nanoseconds =
      RunBenchmark([&] { return TestMutex(thread_count); });
  ASSERT_EQ(counter, kOperations);

  const double shared_mutex_nanoseconds =
      RunBenchmark([&] { return TestSharedMutex(thread_count); });
  ASSERT_EQ(counter, kOperations);

  const double atomic_nanoseconds =
      RunBenchmark([&] { return TestAtomic(thread_count); });
  ASSERT_EQ(atomic_counter.load(std::memory_order_relaxed), kOperations);

  const double atomic_cas_nanoseconds =
      RunBenchmark([&] { return TestAtomicCAS(thread_count); });
  ASSERT_EQ(atomic_counter.load(std::memory_order_relaxed), kOperations);

  // 将一个值写入全局 sink，确保 benchmark 结果没有被优化掉。
  benchmark_sink.fetch_xor(
      static_cast<u64>(mutex_nanoseconds +
                                 shared_mutex_nanoseconds +
                                 atomic_nanoseconds + atomic_cas_nanoseconds),
      std::memory_order_relaxed);

  PrintResult("mutex", mutex_nanoseconds);
  PrintResult("shared mutex", shared_mutex_nanoseconds);
  PrintResult("atomic", atomic_nanoseconds);
  PrintResult("atomic CAS", atomic_cas_nanoseconds);
}

INSTANTIATE_TEST_SUITE_P(
    ThreadCounts, LockBenchmarkTest, ::testing::ValuesIn(kThreadCounts),
    [](const ::testing::TestParamInfo<std::size_t>& info) {
      return std::to_string(info.param) + "Threads";
    });

}  // namespace
