#pragma once

#include <droplet/types.h>

#include <droplet/export.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace droplet {

class TimerManager;

/**
 * @brief 一个基于 steady_clock 的定时器。
 *
 * Timer 由 TimerManager 创建和持有。普通定时器触发一次后自动失效；
 * recurring 定时器每次触发后按照原间隔重新计算下一次到期时间。
 */
class DROPLET_API Timer final
    : public std::enable_shared_from_this<Timer> {
 public:
  using Ptr = std::shared_ptr<Timer>;

  /// 取消定时器；成功取消返回 true，已经触发或取消返回 false。
  bool cancel();
  /// 从当前时间重新开始计时。
  bool refresh();
  /// 修改间隔；from_now 为 true 时从当前时间重新计算起点。
  bool reset(u64 ms, bool from_now);

  [[nodiscard]] u64 getMs() const noexcept {
    return ms_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool isRecurring() const noexcept { return recurring_; }

 private:
  friend class TimerManager;

  Timer(TimerManager* manager, u64 ms, std::function<void()> cb,
        bool recurring, u64 next, u64 sequence)
      : manager_(manager),
        ms_(ms),
        next_(next),
        sequence_(sequence),
        recurring_(recurring),
        cb_(std::move(cb)) {}

  std::atomic<TimerManager*> manager_;
  std::atomic<u64> ms_;
  u64 next_;
  u64 sequence_;
  bool recurring_;
  std::atomic<bool> canceled_{false};
  std::function<void()> cb_;
};

/**
 * @brief 定时器管理器。
 *
 * 定时器按到期时间排序，所有公开操作都是线程安全的。IOManager 继承此
 * 类后会在最早定时器变化时唤醒 epoll_wait，并在 idle 中取出到期回调。
 */
class DROPLET_API TimerManager {
 public:
  using TimerPtr = Timer::Ptr;

  TimerManager() = default;
  virtual ~TimerManager();

  /// 添加定时器。ms 可以为 0，表示下一次调度循环尽快执行。
  [[nodiscard]] TimerPtr addTimer(u64 ms, std::function<void()> cb,
                                  bool recurring = false);

  /// 只有 condition 仍然存活时才执行回调。
  [[nodiscard]] TimerPtr addConditionTimer(
      u64 ms, std::function<void()> cb, std::weak_ptr<void> condition,
      bool recurring = false);

  /// 距离最近定时器到期的毫秒数；没有定时器返回 UINT64_MAX。
  [[nodiscard]] u64 getNextTimer() const;

  /// 把已经到期的回调追加到 cbs；回调在锁外执行。
  void listExpiredCallbacks(std::vector<std::function<void()>>& cbs);
  /// Sylar 风格的兼容命名。
  void listExpiredCb(std::vector<std::function<void()>>& cbs) {
    listExpiredCallbacks(cbs);
  }

  [[nodiscard]] bool hasTimer() const;

 protected:
  /// 最早定时器发生变化时通知事件循环；默认空实现。
  virtual void onTimerInsertedAtFront();

  /// 析构派生事件循环前清空定时器，避免停止时仍被定时器阻塞。
  void clearTimers() noexcept;

 private:
  friend class Timer;

  struct TimerComparator {
    bool operator()(const TimerPtr& lhs, const TimerPtr& rhs) const noexcept {
      if (lhs->next_ != rhs->next_) {
        return lhs->next_ < rhs->next_;
      }
      return lhs->sequence_ < rhs->sequence_;
    }
  };

  bool cancelTimer(Timer& timer);
  bool refreshTimer(Timer& timer);
  bool resetTimer(Timer& timer, u64 ms, bool from_now);

  mutable std::mutex timer_mutex_;
  std::set<TimerPtr, TimerComparator> timers_;
  u64 next_sequence_{0};
};

}  // namespace droplet
