#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(__arm__)
#include <arm_acle.h>
#endif

#if defined(__linux__) && defined(__GLIBC__)
#include <pthread.h>
#endif

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __yield();
#endif
}

namespace droplet {
#if defined(__linux__) && defined(__GLIBC__)

class Spinlock {
 public:
  Spinlock() noexcept { pthread_spin_init(&mutex_, PTHREAD_PROCESS_PRIVATE); }
  ~Spinlock() noexcept { pthread_spin_destroy(&mutex_); }

  Spinlock(const Spinlock&) = delete;
  Spinlock& operator=(const Spinlock&) = delete;

  void lock() noexcept { pthread_spin_lock(&mutex_); }
  void unlock() noexcept { pthread_spin_unlock(&mutex_); }

 private:
  pthread_spinlock_t mutex_{};
};
#else

class Spinlock {
 public:
  using Lock = std::lock_guard<Spinlock>;

  Spinlock() noexcept = default;
  ~Spinlock() noexcept = default;

  Spinlock(const Spinlock&) = delete;
  Spinlock& operator=(const Spinlock&) = delete;

  void lock() noexcept {
    while (mutex_.test_and_set(std::memory_order_acquire)) {
      while (mutex_.test(std::memory_order_relaxed)) {
        cpu_relax();
      }
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    return !mutex_.test_and_set(std::memory_order_acquire);
  }

  void unlock() noexcept { mutex_.clear(std::memory_order_release); }

 private:
  std::atomic_flag mutex_ = ATOMIC_FLAG_INIT;
};

#endif

class AtomicWaitLock {
 public:
  using Lock = std::lock_guard<AtomicWaitLock>;

  AtomicWaitLock() noexcept = default;
  ~AtomicWaitLock() noexcept = default;

  AtomicWaitLock(const AtomicWaitLock&) = delete;
  AtomicWaitLock& operator=(const AtomicWaitLock&) = delete;

  void lock() noexcept {
    bool expected = false;
    while (!mutex_.compare_exchange_weak(
        expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
      std::atomic_wait_explicit(&mutex_, true, std::memory_order_relaxed);
      expected = false;
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    bool expected = false;
    return mutex_.compare_exchange_strong(
        expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept {
    mutex_.store(false, std::memory_order_release);
    std::atomic_notify_one(&mutex_);
  }

 private:
  std::atomic<bool> mutex_{false};
};

class ThreeStageLock {
 public:
  using Lock = std::lock_guard<ThreeStageLock>;

  void lock() noexcept {
    constexpr unsigned kSpinLimit = 64;
    constexpr unsigned kYieldLimit = 8;

    unsigned spin_count = 0;
    unsigned yield_count = 0;

    for (;;) {
      // 先只读，避免锁被占用时反复执行 CAS 写操作。
      if (!locked_.load(std::memory_order_relaxed)) {
        bool expected = false;

        if (locked_.compare_exchange_weak(expected, true,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
          return;
        }
      }

      // 第一阶段：短暂自旋。
      if (spin_count < kSpinLimit) {
        ++spin_count;
        cpu_relax();
        continue;
      }

      // 第二阶段：主动让出当前时间片。
      if (yield_count < kYieldLimit) {
        ++yield_count;
        std::this_thread::yield();
        continue;
      }

      // 第三阶段：阻塞等待锁释放。
      std::atomic_wait_explicit(&locked_, true, std::memory_order_relaxed);

      // 被唤醒后重新开始竞争。
      spin_count = 0;
      yield_count = 0;
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    bool expected = false;
    return locked_.compare_exchange_strong(
        expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept {
    locked_.store(false, std::memory_order_release);
    std::atomic_notify_one(&locked_);
  }

 private:
  std::atomic<bool> locked_{false};
};

}  // namespace droplet