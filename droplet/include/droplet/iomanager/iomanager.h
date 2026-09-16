#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/scheduler/scheduler.h>
#include <droplet/timer/timer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace droplet {

/**
 * @brief 基于 epoll 的 I/O 协程调度器。
 *
 * IOManager 在 Scheduler 的任务队列之上增加 fd 事件和定时器：空闲协程
 * 不再等待条件变量，而是进入 epoll_wait；fd 就绪或定时器到期后，对应的
 * Fiber/回调被重新投递到 Scheduler 执行。
 *
 * 当前实现面向 Linux（epoll + eventfd）。事件采用水平触发，事件处理后
 * 会自动移除该 READ/WRITE 注册；如果需要继续等待，回调或协程应重新
 * 调用 addEvent。
 */
class DROPLET_API IOManager final : public Scheduler, public TimerManager {
 public:
  using Ptr = std::shared_ptr<IOManager>;
  using EventMask = u32;

  static constexpr EventMask NONE = 0;
  static constexpr EventMask READ = 1u << 0;
  // 与 epoll/Sylar 的事件值保持一致，便于直接阅读调试输出。
  static constexpr EventMask WRITE = 1u << 2;

  /**
   * @param[in] thread_count 调度线程总数；use_caller 时包含调用线程。
   * @param[in] use_caller 是否让调用线程进入调度循环。
   * @param[in] name 调度器名称。
   */
  explicit IOManager(size_t thread_count = 1, bool use_caller = true,
                     std::string name = "");
  ~IOManager() override;

  /**
   * @brief 注册一个 fd 事件。
   * @param[in] fd 文件描述符。
   * @param[in] event 只能是 READ 或 WRITE 之一。
   * @param[in] cb 非空时事件就绪后执行回调；为空时恢复当前 Fiber。
   * @return 成功返回 0，失败返回 -1 并设置 errno。
   *
   * 空回调模式要求当前运行在有独立栈的 Fiber 中，并且该 Fiber 在
   * addEvent 返回后主动调用 Fiber::yield_to_hold()。
   */
  int addEvent(int fd, EventMask event, std::function<void()> cb = {});

  /// 删除事件但不执行其回调，也不会恢复等待中的 Fiber。
  bool delEvent(int fd, EventMask event);
  /// 删除事件并执行其回调或恢复等待中的 Fiber。
  bool cancelEvent(int fd, EventMask event);
  /// 删除 fd 上的所有事件，并执行所有对应回调/恢复所有 Fiber。
  bool cancelAll(int fd);

  [[nodiscard]] size_t pendingEventCount() const noexcept {
    return pending_event_count_.load(std::memory_order_acquire);
  }
  [[nodiscard]] size_t getPendingEventCount() const noexcept {
    return pendingEventCount();
  }

  /// 当前线程所在的 IOManager；不在 IOManager 调度线程时返回 nullptr。
  [[nodiscard]] static IOManager* GetThis() noexcept;

 protected:
  void tickle() override;
  [[nodiscard]] bool stopping() const override;
  void idle() override;
  void onTimerInsertedAtFront() override;

 private:
  struct EventContext {
    Fiber::Ptr fiber;
    std::function<void()> cb;

    [[nodiscard]] bool empty() const noexcept {
      return !fiber && !cb;
    }
    void clear() noexcept {
      fiber.reset();
      cb = nullptr;
    }
  };

  struct FdContext {
    explicit FdContext(int value) : fd(value) {}

    int fd;
    EventMask events{NONE};
    EventContext read;
    EventContext write;
    std::mutex mutex;
  };

  static constexpr size_t kMaxEpollEvents = 64;
  static constexpr int kMaxTrackedFd = 1 << 20;
  static constexpr u64 kTickleTag = 1;

  FdContext* getFdContext(int fd, bool auto_create);
  bool updateEpoll(FdContext& context, EventMask events);
  static u32 toEpollEvents(EventMask events) noexcept;
  static bool validEvent(EventMask event) noexcept;

  void trigger(EventContext&& context);
  bool processEvent(const void* epoll_data, u32 epoll_events);
  void drainTickle() noexcept;
  void clearAllEventsWithoutResume() noexcept;

  int epoll_fd_{-1};
  int tickle_fd_{-1};
  u64 wake_count_{1};
  mutable std::mutex fd_contexts_mutex_;
  std::vector<std::unique_ptr<FdContext>> fd_contexts_;
  std::atomic<size_t> pending_event_count_{0};
  std::atomic<bool> force_stop_{false};
};

}  // namespace droplet
