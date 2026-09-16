#include "droplet/iomanager/iomanager.h"
#include <droplet/types.h>

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <climits>
#include <cerrno>
#include <limits>

namespace droplet {

void IOManager::drainTickle() noexcept {
  u64 value = 0;
  while (::read(tickle_fd_, &value, sizeof(value)) == -1 && errno == EINTR) {
  }
}

void IOManager::idle() {
  std::array<epoll_event, kMaxEpollEvents> events{};

  while (true) {
    const u64 next_timer = getNextTimer();
    int timeout = -1;
    if (next_timer != std::numeric_limits<u64>::max()) {
      timeout = next_timer > static_cast<u64>(INT_MAX)
                    ? INT_MAX
                    : static_cast<int>(next_timer);
    }

    const int count = ::epoll_wait(epoll_fd_, events.data(),
                                   static_cast<int>(events.size()), timeout);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return;
    }

    bool ready = false;
    for (int i = 0; i < count; ++i) {
      if (events[static_cast<size_t>(i)].data.u64 == kTickleTag) {
        drainTickle();
        // tickle 可能是 schedule() 写入的；必须把控制权交回 run()，
        // 否则 idle 会直接再次 epoll_wait，看不到 Scheduler::tasks_。
        ready = true;
      } else if (processEvent(events[static_cast<size_t>(i)].data.ptr,
                              events[static_cast<size_t>(i)].events)) {
        ready = true;
      }
    }

    std::vector<std::function<void()>> expired;
    listExpiredCallbacks(expired);
    if (!expired.empty()) {
      ready = true;
      for (auto& cb : expired) {
        schedule(std::move(cb));
      }
    }

    if (stopping()) {
      return;
    }
    if (ready) {
      // 让 run() 先执行刚刚投递的任务；任务耗尽后会再次 resume idle。
      Fiber::yield_to_hold();
    }
  }
}

void IOManager::tickle() {
  if (tickle_fd_ == -1) {
    return;
  }
  // 普通投递唤醒一个 worker；stop() 已经先设置 stopping_，此时写入
  // 一个 token 给每个可能阻塞在 epoll_wait 的调度线程。
  const u64 value = Scheduler::stopping() ? wake_count_ : 1;
  const ssize_t result = ::write(tickle_fd_, &value, sizeof(value));
  if (result < 0 && errno != EAGAIN && errno != EINTR) {
    // 唤醒是尽力而为；eventfd 已有计数时 EAGAIN 不影响正确性。
  }
}

bool IOManager::stopping() const {
  if (!Scheduler::stopping()) {
    return false;
  }
  if (force_stop_.load(std::memory_order_acquire)) {
    return true;
  }
  if (pendingEventCount() != 0 || hasTimer()) {
    return false;
  }

  // Scheduler::stop() 仍然具有排空任务队列的语义；不能因为没有 fd/定时器
  // 就提前结束 idle，否则已经入队的回调会被丢弃。
  std::lock_guard<MutexType> lock(mutex_);
  return tasks_.empty();
}

void IOManager::onTimerInsertedAtFront() { tickle(); }

void IOManager::clearAllEventsWithoutResume() noexcept {
  std::lock_guard<std::mutex> contexts_lock(fd_contexts_mutex_);
  for (auto& holder : fd_contexts_) {
    if (!holder) {
      continue;
    }
    FdContext& context = *holder;
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.events != NONE) {
      (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, context.fd, nullptr);
    }
    context.events = NONE;
    context.read.clear();
    context.write.clear();
  }
  pending_event_count_.store(0, std::memory_order_release);
}

}  // namespace droplet
