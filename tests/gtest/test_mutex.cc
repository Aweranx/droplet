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
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <droplet/mutex.h>

namespace {

constexpr int kRuns = 7;
constexpr u64 kOperations = 1'000'000;
constexpr std::array<std::size_t, 5> kThreadCounts = {1, 2, 4, 8, 10};
// 0 表示短临界区，100 用于模拟较长的临界区。
constexpr std::array<unsigned, 2> kWorkRounds = {0, 100};
using MutexTestParam = std::tuple<std::size_t, unsigned>;

// 测试文件中的 atomic_flag 自旋锁，用来和库里的锁实现进行对比。
class AtomicFlagSpinlock {
 public:
  using Lock = std::lock_guard<AtomicFlagSpinlock>;

  AtomicFlagSpinlock() noexcept = default;
  ~AtomicFlagSpinlock() noexcept = default;

  AtomicFlagSpinlock(const AtomicFlagSpinlock&) = delete;
  AtomicFlagSpinlock& operator=(const AtomicFlagSpinlock&) = delete;

  void lock() noexcept {
    while (mutex_.test_and_set(std::memory_order_acquire)) {
      while (mutex_.test(std::memory_order_relaxed)) {
        cpu_relax();
      }
    }
  }

  void unlock() noexcept { mutex_.clear(std::memory_order_release); }

 private:
  std::atomic_flag mutex_ = ATOMIC_FLAG_INIT;
};

u64 BusyWork(u64 value, unsigned rounds) noexcept {
  for (unsigned index = 0; index < rounds; ++index) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
  }
  return value;
}

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

struct BenchmarkResult {
  double average_nanoseconds = 0.0;
  bool counter_is_correct = false;
  u64 work_sink = 0;
};

template <typename Mutex>
BenchmarkResult MeasureMutex(std::size_t thread_count,
                             unsigned work_rounds) {
  Mutex mutex;
  u64 counter = 0;
  u64 work_sink = 0;
  long double total_nanoseconds = 0.0L;
  bool counter_is_correct = true;

  for (int run = 0; run < kRuns; ++run) {
    counter = 0;
    work_sink = 0;
    const auto elapsed = BenchmarkThreads(
        thread_count,
        [&mutex, &counter, &work_sink,
         work_rounds](std::size_t thread_index, u64 operations) {
          for (u64 index = 0; index < operations; ++index) {
            std::lock_guard<Mutex> lock(mutex);
            if (work_rounds != 0) {
              work_sink = BusyWork(
                  work_sink ^ static_cast<u64>(thread_index) ^ index,
                  work_rounds);
            }
            ++counter;
          }
        });
    total_nanoseconds += static_cast<long double>(elapsed.count());
    counter_is_correct = counter_is_correct && counter == kOperations;
  }

  return {static_cast<double>(total_nanoseconds / kRuns), counter_is_correct,
          work_sink};
}

void PrintResult(std::string_view name, double total_nanoseconds) {
  std::println("{:<28}: {:.2f} ns , {:.2f} ns/op", name,
               total_nanoseconds,
               total_nanoseconds / static_cast<double>(kOperations));
}

// 线程数和临界区工作量组合成一个参数，因此每个组合生成一个用例。
class MutexBenchmarkTest
    : public ::testing::TestWithParam<MutexTestParam> {};

TEST_P(MutexBenchmarkTest, ComparesLockImplementations) {
  const auto [thread_count, work_rounds] = GetParam();
  std::print("operations: {}, average of {} runs, work rounds: {}\n",
             kOperations, kRuns, work_rounds);
  std::println("{} threads", thread_count);

  const auto pthread_spinlock =
      MeasureMutex<droplet::Spinlock>(thread_count, work_rounds);
  const auto atomic_flag_spinlock =
      MeasureMutex<AtomicFlagSpinlock>(thread_count, work_rounds);
  const auto atomic_wait =
      MeasureMutex<droplet::AtomicWaitLock>(thread_count, work_rounds);
  const auto three_stage =
      MeasureMutex<droplet::ThreeStageLock>(thread_count, work_rounds);
  const auto std_mutex = MeasureMutex<std::mutex>(thread_count, work_rounds);

  ASSERT_TRUE(pthread_spinlock.counter_is_correct);
  ASSERT_TRUE(atomic_flag_spinlock.counter_is_correct);
  ASSERT_TRUE(atomic_wait.counter_is_correct);
  ASSERT_TRUE(three_stage.counter_is_correct);
  ASSERT_TRUE(std_mutex.counter_is_correct);

  PrintResult("pthread_spinlock::Spinlock", pthread_spinlock.average_nanoseconds);
  PrintResult("atomic_flag::Spinlock", atomic_flag_spinlock.average_nanoseconds);
  PrintResult("atomic_wait", atomic_wait.average_nanoseconds);
  PrintResult("ThreeStageLock", three_stage.average_nanoseconds);
  PrintResult("std::mutex", std_mutex.average_nanoseconds);

  // 保留 work sink，确保长临界区计算没有被优化掉。
  std::println("work sink: {}", std_mutex.work_sink);
}

INSTANTIATE_TEST_SUITE_P(
    ThreadAndWork, MutexBenchmarkTest,
    ::testing::Combine(::testing::ValuesIn(kThreadCounts),
                       ::testing::ValuesIn(kWorkRounds)),
    [](const ::testing::TestParamInfo<MutexTestParam>& info) {
      return std::to_string(std::get<0>(info.param)) + "Threads_" +
             std::to_string(std::get<1>(info.param)) + "Rounds";
    });

}  // namespace
