#pragma once

#include <droplet/export.h>
#include <droplet/fiber/fiber.h>
#include <droplet/macros.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <latch>

namespace droplet {

/**
 * @brief 协程调度器：N 个线程上跑 M 个协程的简单调度。
 * @details 每个工作线程运行同一个主循环：从任务队列取协程（或函数）→
 *          resume 执行 → 按让出后的状态决定去向：
 *          - READY（yield_to_ready）：重新入队，尽快再次运行；
 *          - HOLD（yield_to_hold）：不再自动调度，等待外部 resume
 *            （IOManager 接管后由 fd 事件恢复）；
 *          - TERM/EXCEPT：结束，释放。
 *
 *          空闲线程跑 idle 协程等待新任务；tickle() 是唤醒钩子，
 *          基类用条件变量实现，IOManager 将重写为"写 eventfd 踹醒
 *          epoll_wait"。idle()/tickle()/stopping() 均为虚函数，
 *          预留给 IOManager 重写。
 *
 *          使用约束：
 *          - stop() 不能在 worker 线程上调用（join 自己死锁）；
 *            use_caller 时可以在 caller 线程（含其协程内）调用；
 *          - schedule(function) 包装的协程只有在外部对象（例如 IOManager
 *            的 fd 事件上下文）持有 Fiber 时才能 yield_to_hold；普通
 *            Scheduler 没有这样的持有者，不能让出后再恢复；
 *          - 指定线程（thread_id）的任务在对应线程退出后不再执行。
 */
class DROPLET_API Scheduler {
 public:
  using Ptr = std::shared_ptr<Scheduler>;
  using MutexType = std::mutex;

  /**
   * @brief 构造调度器
   * @param[in] thread_count 调度线程总数（use_caller 时含调用线程自身，
   *                         额外创建的 worker 数为 thread_count - 1）
   * @param[in] use_caller   调用线程是否作为调度线程之一：start() 时
   *                         调用线程直接进入调度循环并阻塞到 stop()
   * @param[in] name         调度器名称（日志用）
   */
  Scheduler(size_t thread_count, bool use_caller = true, std::string name = "");
  virtual ~Scheduler();

  /// 当前线程所在的调度器；未进入调度循环时返回 nullptr。
  [[nodiscard]] static Scheduler* GetThis();

  [[nodiscard]] const std::string& getName() const noexcept { return name_; }

  /// 启动调度：创建 worker 线程；use_caller 时本线程进入调度循环
  /// （阻塞到 stop）。start 返回前所有调度线程已注册，可安全使用
  /// schedule(fc, thread_id) 指定线程。
  void start();

  /// 停止调度：排空队列后所有调度线程退出。幂等；use_caller 时
  /// start() 在此之后返回。
  void stop();

  /// 投递协程任务；thread_id = -1 表示任意线程，否则 pin 到对应线程
  /// （id 取 getThreadIds() 中的值）。
  void schedule(Fiber::Ptr fiber, int64_t thread_id = -1);
  /// 投递函数任务：内部包装为协程执行。
  void schedule(std::function<void()> cb, int64_t thread_id = -1);

  /// 批量投递（元素为 Fiber::Ptr 或 std::function<void()>）。
  template <class Iter>
  void schedule(Iter begin, Iter end) {
    bool need_tickle = false;
    {
      std::lock_guard<MutexType> lock(mutex_);
      for (; begin != end; ++begin) {
        if constexpr (std::is_same_v<std::decay_t<decltype(*begin)>,
                                     Fiber::Ptr>) {
          tasks_.push_back(ScheduleTask{*begin, nullptr, -1});
        } else {
          tasks_.push_back(ScheduleTask{nullptr, *begin, -1});
        }
        need_tickle = true;
      }
    }
    if (need_tickle) {
      cv_.notify_one();
      tickle();
    }
  }

  /// 全部调度线程 id（含 use_caller 时的调用线程），供 schedule 指定线程。
  [[nodiscard]] const std::vector<uint64_t>& getThreadIds() const noexcept {
    return thread_ids_;
  }

 protected:
  /// 队列元素：协程与函数二选一，thread_id 为 -1 表示任意线程。
  struct ScheduleTask {
    Fiber::Ptr fiber;
    std::function<void()> cb;
    int64_t thread_id;
  };

  /// 唤醒空闲线程的钩子；基类空实现（条件变量已负责唤醒），预留给
  /// IOManager 重写（写 eventfd 踹醒 epoll_wait）。
  virtual void tickle();

  /// 是否处于停止流程；IOManager 可附加条件（如剩余定时器未到期）。
  [[nodiscard]] virtual bool stopping() const;

  /// 空闲协程：等待任务或停止；IOManager 将重写为 epoll_wait 循环。
  virtual void idle();

  /// 登记当前线程的调度器（GetThis 的来源）。
  void setThis() noexcept;

  /// 调度线程主循环，所有 worker 与 use_caller 的调用线程共用。
  void run();

  /// 取出一个可执行任务：优先本线程 pin 的，其次任意线程的；
  /// 正在 EXEC 的协程跳过（防重复调度）。
  bool getNextTask(ScheduleTask& out);

  mutable MutexType mutex_;        ///< 保护 tasks_ / thread_ids_ / registered_
  size_t worker_count_{0};         ///< 额外创建的 worker 数
  std::condition_variable cv_;     ///< 唤醒 idle 协程
  std::latch registered_latch_;   ///< start 等待全部 worker 注册
  std::list<ScheduleTask> tasks_;  ///< 待调度任务
  std::vector<std::thread> threads_;       ///< worker 线程（不含 caller）
  std::vector<uint64_t> thread_ids_;       ///< 全部调度线程 id
  
  size_t registered_{0};           ///< 已注册 id 的调度线程数
  bool use_caller_{false};
  bool started_{false};
  std::string name_;
  uint64_t caller_thread_id_{0};   ///< use_caller 时调用线程的 id
  std::atomic<bool> stopping_{false};

 private:
  static thread_local Scheduler* t_scheduler_;
};

}  // namespace droplet
