#include "droplet/scheduler/scheduler.h"

#include <droplet/utils/thread_utils.h>

#include <utility>

namespace droplet {

thread_local Scheduler* Scheduler::t_scheduler_ = nullptr;

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Scheduler::Scheduler(size_t thread_count, bool use_caller, std::string name)
    : worker_count_(use_caller ? (thread_count > 0 ? thread_count - 1 : 0)
                               : thread_count),
      registered_latch_(worker_count_),
      use_caller_(use_caller),
      name_(std::move(name)) {
  DROPLET_ASSERT(thread_count > 0);
  if (use_caller_) {
    // 调用线程本身就是调度线程之一：登记其 id，start 时直接进调度循环。
    caller_thread_id_ = GetThreadId();
    thread_ids_.push_back(caller_thread_id_);
    registered_ = 1;
  }
}

Scheduler::~Scheduler() {
  // 兜底回收：用户忘记 stop 时在这里停掉线程，正常路径 stop 已 join。
  if (started_ && !stopping_.exchange(true)) {
    cv_.notify_all();
    for (auto& t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
    threads_.clear();
  }
}

void Scheduler::setThis() noexcept { t_scheduler_ = this; }

Scheduler* Scheduler::GetThis() { return t_scheduler_; }

}  // namespace droplet
