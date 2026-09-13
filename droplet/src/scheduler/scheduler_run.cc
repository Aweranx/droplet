#include "droplet/scheduler/scheduler.h"

#include <droplet/hook/hook.h>
#include <utility>

namespace droplet {

void Scheduler::run() {
  setThis();
  // Sylar 风格：调度线程中的阻塞系统调用由 hook 转换为 Fiber 等待。
  // 线程离开调度循环前关闭，避免 hook 影响该线程的后续普通代码。
  setHookEnabled(true);

  // 每个调度线程一个 idle 协程：可反复 resume（HOLD），仅在退出时 TERM。
  Fiber::Ptr idle_fiber = Fiber::Create([this] { idle(); });
  // 复用的函数任务协程：TERM 后 reset 下一个 cb，避免反复分配栈。
  Fiber::Ptr cb_fiber;

  while (true) {
    ScheduleTask task;
    if (!getNextTask(task)) {
      // 无任务可跑：进 idle 协程等待；它 TERM（停止且队列空）时退出循环。
      idle_fiber->resume();
      if (idle_fiber->getState() == Fiber::State::TERM) {
        break;
      }
      continue;
    }

    if (task.fiber) {
      task.fiber->resume();
      if (task.fiber->getState() == Fiber::State::READY) {
        // yield_to_ready：还想再跑，重新入队（保持原线程偏好）。
        schedule(std::move(task.fiber), task.thread_id);
      }
      // HOLD：调度器不再管，恢复责任在外部持有者（如 IOManager）；
      // TERM/EXCEPT：自然结束。本地引用交还后对象归外部引用所有。
      task.fiber = nullptr;
      continue;
    }

    // 函数任务：包装为协程执行；cb_fiber 跨任务复用（reset 复用旧栈）。
    if (!cb_fiber) {
      cb_fiber = Fiber::Create(nullptr);
    }
    if (cb_fiber->getState() == Fiber::State::TERM) {
      cb_fiber->reset(std::move(task.cb));
    } else {
      // 上一个函数任务 yield_to_ready 后回队（cb_fiber 已被队列接管）。
      cb_fiber = Fiber::Create(std::move(task.cb));
    }
    cb_fiber->resume();
    if (cb_fiber->getState() == Fiber::State::READY) {
      // 让出想再跑：入队，队列接管生命周期，下轮换新协程。
      schedule(std::move(cb_fiber));
    } else if (cb_fiber->getState() == Fiber::State::HOLD) {
      // IOManager 的 addEvent(cb={}) 会把当前函数协程交给事件上下文持有，
      // 因此 HOLD 不能在这里断言；事件触发后会重新 schedule 该 Fiber。
      cb_fiber = nullptr;
    }
  }

  // 线程离开调度循环后不再是调度线程，清理登记。
  if (t_scheduler_ == this) {
    t_scheduler_ = nullptr;
  }
  setHookEnabled(false);
}

void Scheduler::idle() {
  while (true) {
    {
      std::unique_lock<MutexType> lock(mutex_);
      // 基类用条件变量睡眠；IOManager 重写 idle 后这里是 epoll_wait，
      // 同时承担定时器超时与新 IO 事件的等待。
      cv_.wait(lock, [this] { return stopping_.load() || !tasks_.empty(); });
    }
    if (stopping_.load() && tasks_.empty()) {
      return;  // 结束 idle 协程（TERM）→ run 循环随之退出
    }
    Fiber::yield_to_hold();  // 有任务了：让出回 run 循环去取
  }
}

void Scheduler::tickle() {
  // 基类无需额外动作（条件变量已在 schedule 时 notify）；
  // IOManager 重写为向 eventfd/pipe 写一个字节，踹醒 epoll_wait。
}

bool Scheduler::stopping() const { return stopping_.load(); }

}  // namespace droplet
