#include "droplet/scheduler/scheduler.h"
#include <droplet/types.h>

#include <droplet/utils/thread_utils.h>

#include <utility>

namespace droplet {

void Scheduler::schedule(Fiber::Ptr fiber, i64 thread_id) {
  DROPLET_ASSERT(fiber);
  // 注意：允许在 stopping 后投递——run 循环对 yield_to_ready 协程的
  // 重新入队就发生在停止流程中；停止后仍未执行的任务会被丢弃。
  {
    std::lock_guard<MutexType> lock(mutex_);
    tasks_.push_back(ScheduleTask{std::move(fiber), nullptr, thread_id});
  }
  if (thread_id == -1) {
    cv_.notify_one();
  } else {
    cv_.notify_all();
  }
  tickle();
}

void Scheduler::schedule(std::function<void()> cb, i64 thread_id) {
  DROPLET_ASSERT(cb);
  {
    std::lock_guard<MutexType> lock(mutex_);
    tasks_.push_back(ScheduleTask{nullptr, std::move(cb), thread_id});
  }
  if (thread_id == -1) {
    cv_.notify_one();
  } else {
    cv_.notify_all();
  }
  tickle();
}

bool Scheduler::getNextTask(ScheduleTask& out) {
  std::lock_guard<MutexType> lock(mutex_);
  if (tasks_.empty()) {
    return false;
  }
  const auto self = static_cast<i64>(GetThreadId());
  for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
    // 跳过别人的 pinned 任务；正在 EXEC 的协程防重复调度，留在原地。
    if (it->thread_id != -1 && it->thread_id != self) {
      continue;
    }
    if (it->fiber && it->fiber->getState() == Fiber::State::EXEC) {
      continue;
    }
    out = std::move(*it);
    tasks_.erase(it);
    return true;
  }
  return false;
}

}  // namespace droplet
