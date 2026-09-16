#include "droplet/timer/timer.h"
#include <droplet/types.h>

#include <droplet/macros.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace droplet {

namespace {

u64 NowMilliseconds() noexcept {
  using namespace std::chrono;
  return static_cast<u64>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

bool Timer::cancel() {
  TimerManager* manager = manager_.load(std::memory_order_acquire);
  return manager && manager->cancelTimer(*this);
}

bool Timer::refresh() {
  TimerManager* manager = manager_.load(std::memory_order_acquire);
  return manager && manager->refreshTimer(*this);
}

bool Timer::reset(u64 ms, bool from_now) {
  TimerManager* manager = manager_.load(std::memory_order_acquire);
  return manager && manager->resetTimer(*this, ms, from_now);
}

TimerManager::~TimerManager() { clearTimers(); }

TimerManager::TimerPtr TimerManager::addTimer(u64 ms,
                                              std::function<void()> cb,
                                              bool recurring) {
  DROPLET_ASSERT(cb);
  if (!cb) {
    return nullptr;
  }

  const u64 now = NowMilliseconds();
  TimerPtr timer;
  bool at_front = false;
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    timer = TimerPtr(new Timer(this, ms, std::move(cb), recurring, now + ms,
                               next_sequence_++));
    at_front = timers_.empty() || TimerComparator{}(timer, *timers_.begin());
    timers_.insert(timer);
  }
  if (at_front) {
    onTimerInsertedAtFront();
  }
  return timer;
}

TimerManager::TimerPtr TimerManager::addConditionTimer(
    u64 ms, std::function<void()> cb, std::weak_ptr<void> condition,
    bool recurring) {
  DROPLET_ASSERT(cb);
  if (!cb) {
    return nullptr;
  }

  return addTimer(
      ms,
      [condition = std::move(condition), cb = std::move(cb)]() mutable {
        if (condition.lock()) {
          cb();
        }
      },
      recurring);
}

u64 TimerManager::getNextTimer() const {
  std::lock_guard<std::mutex> lock(timer_mutex_);
  if (timers_.empty()) {
    return std::numeric_limits<u64>::max();
  }

  const u64 now = NowMilliseconds();
  const u64 next = (*timers_.begin())->next_;
  return next <= now ? 0 : next - now;
}

void TimerManager::listExpiredCallbacks(
    std::vector<std::function<void()>>& cbs) {
  const u64 now = NowMilliseconds();

  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    while (!timers_.empty()) {
      auto it = timers_.begin();
      TimerPtr timer = *it;
      if (timer->next_ > now) {
        break;
      }

      timers_.erase(it);
      if (timer->canceled_.load(std::memory_order_acquire)) {
        continue;
      }

      if (timer->recurring_) {
        // 0ms recurring timer 也必须向前推进，否则同一次扫描会无限
        // 重新命中同一个定时器。
        timer->next_ =
            now + std::max<u64>(timer->ms_.load(std::memory_order_acquire),
                                     1);
        timers_.insert(timer);
        cbs.push_back(timer->cb_);
      } else {
        timer->canceled_.store(true, std::memory_order_release);
        timer->manager_.store(nullptr, std::memory_order_release);
        cbs.push_back(std::move(timer->cb_));
      }
    }
  }
}

bool TimerManager::hasTimer() const {
  std::lock_guard<std::mutex> lock(timer_mutex_);
  return !timers_.empty();
}

void TimerManager::onTimerInsertedAtFront() {}

void TimerManager::clearTimers() noexcept {
  std::lock_guard<std::mutex> lock(timer_mutex_);
  for (auto& timer : timers_) {
    timer->canceled_.store(true, std::memory_order_release);
    timer->manager_.store(nullptr, std::memory_order_release);
    timer->cb_ = nullptr;
  }
  timers_.clear();
}

bool TimerManager::cancelTimer(Timer& timer) {
  std::lock_guard<std::mutex> lock(timer_mutex_);
  if (timer.manager_.load(std::memory_order_acquire) != this ||
      timer.canceled_.load(std::memory_order_acquire)) {
    return false;
  }

  TimerPtr ptr = timer.shared_from_this();
  auto it = timers_.find(ptr);
  if (it == timers_.end()) {
    return false;
  }

  timers_.erase(it);
  timer.canceled_.store(true, std::memory_order_release);
  timer.manager_.store(nullptr, std::memory_order_release);
  timer.cb_ = nullptr;
  return true;
}

bool TimerManager::refreshTimer(Timer& timer) {
  bool at_front = false;
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (timer.manager_.load(std::memory_order_acquire) != this ||
        timer.canceled_.load(std::memory_order_acquire)) {
      return false;
    }

    TimerPtr ptr = timer.shared_from_this();
    auto it = timers_.find(ptr);
    if (it == timers_.end()) {
      return false;
    }
    timers_.erase(it);
    timer.next_ =
        NowMilliseconds() + timer.ms_.load(std::memory_order_acquire);
    at_front = timers_.empty() || TimerComparator{}(ptr, *timers_.begin());
    timers_.insert(std::move(ptr));
  }

  if (at_front) {
    onTimerInsertedAtFront();
  }
  return true;
}

bool TimerManager::resetTimer(Timer& timer, u64 ms, bool from_now) {
  bool at_front = false;
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (timer.manager_.load(std::memory_order_acquire) != this ||
        timer.canceled_.load(std::memory_order_acquire)) {
      return false;
    }

    TimerPtr ptr = timer.shared_from_this();
    auto it = timers_.find(ptr);
    if (it == timers_.end()) {
      return false;
    }
    timers_.erase(it);

    const u64 old_ms = timer.ms_.load(std::memory_order_acquire);
    const u64 start = from_now ? NowMilliseconds() : timer.next_ - old_ms;
    timer.ms_.store(ms, std::memory_order_release);
    timer.next_ = start + ms;
    at_front = timers_.empty() || TimerComparator{}(ptr, *timers_.begin());
    timers_.insert(std::move(ptr));
  }

  if (at_front) {
    onTimerInsertedAtFront();
  }
  return true;
}

}  // namespace droplet
