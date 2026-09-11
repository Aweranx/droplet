#include "droplet/scheduler/scheduler.h"

#include <droplet/utils/thread_utils.h>

#include <algorithm>

namespace droplet {

void Scheduler::start() {
  DROPLET_ASSERT(!started_ && "start 不能重复调用");
  started_ = true;
  stopping_ = false;

  for (size_t i = 0; i < worker_count_; ++i) {
    threads_.emplace_back([this] {
      {
        // 先登记线程 id，schedule(fc, tid) 的 pin 功能依赖这张表。
        std::lock_guard<MutexType> lock(mutex_);
        thread_ids_.push_back(GetThreadId());
        ++registered_;
      }
      registered_latch_.count_down();
      run();
    });
  }

  if (use_caller_) {
    // 调用线程直接进入调度循环：阻塞直到 stop 后循环退出。
    // 等价 sylar 的 root fiber 方案（CallerMainFunc），这里少了
    // 一层协程包装——循环直接跑在线程栈上，stop 语义见类注释。
    run();
  } else {
    // 等全部 worker 注册完成，保证 start 返回后 pin 到任意线程都有效。
    registered_latch_.wait();
  }
}

void Scheduler::stop() {
  if (stopping_.exchange(true)) {
    return;  // 幂等
  }
  // worker 线程上调用 stop 会 join 自己造成死锁，Debug 下直接断言；
  // use_caller 时 caller 线程（含其协程内）调用是合法的：caller 不在
  // threads_ 里，不会被自己 join。
  const auto self = GetThreadId();
  bool on_worker = false;
  if (self != caller_thread_id_) {
    std::lock_guard<MutexType> lock(mutex_);
    on_worker = std::find(thread_ids_.begin(), thread_ids_.end(), self) !=
                thread_ids_.end();
  }
  DROPLET_ASSERT(!on_worker && "stop 不能在 worker 线程上调用");

  cv_.notify_all();  // 踹醒所有 idle 协程，让它们走退出分支
  tickle();
  for (auto& t : threads_) {
    t.join();
  }
  threads_.clear();
}

}  // namespace droplet
