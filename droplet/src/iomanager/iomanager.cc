#include "droplet/iomanager/iomanager.h"
#include <droplet/types.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <system_error>
#include <utility>

namespace droplet {

IOManager::IOManager(size_t thread_count, bool use_caller, std::string name)
    : Scheduler(thread_count, use_caller, std::move(name)),
      epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)),
      tickle_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE)),
      wake_count_(std::max<size_t>(thread_count, 1)) {
  if (epoll_fd_ == -1) {
    throw std::system_error(errno, std::generic_category(),
                            "epoll_create1 failed");
  }
  if (tickle_fd_ == -1) {
    const int error = errno;
    ::close(epoll_fd_);
    epoll_fd_ = -1;
    throw std::system_error(error, std::generic_category(),
                            "eventfd failed");
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.u64 = kTickleTag;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tickle_fd_, &event) != 0) {
    const int error = errno;
    ::close(tickle_fd_);
    ::close(epoll_fd_);
    tickle_fd_ = -1;
    epoll_fd_ = -1;
    throw std::system_error(error, std::generic_category(),
                            "epoll_ctl add eventfd failed");
  }
}

IOManager::~IOManager() {
  // 析构是强制收尾路径：先让 idle 退出并 join，避免清理 fd 上下文/定时器
  // 时仍有 worker 并发访问。正常显式 stop() 仍保留“等待事件自然结束”的
  // Sylar 语义；析构则不应因为用户忘记取消事件而永久阻塞。
  force_stop_.store(true, std::memory_order_release);
  Scheduler::stop();
  clearAllEventsWithoutResume();
  clearTimers();

  if (tickle_fd_ != -1) {
    ::close(tickle_fd_);
    tickle_fd_ = -1;
  }
  if (epoll_fd_ != -1) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

IOManager* IOManager::GetThis() noexcept {
  return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

bool IOManager::validEvent(EventMask event) noexcept {
  return event == READ || event == WRITE;
}

u32 IOManager::toEpollEvents(EventMask events) noexcept {
  u32 result = 0;
  if (events & READ) {
    result |= EPOLLIN;
  }
  if (events & WRITE) {
    result |= EPOLLOUT;
  }
  return result;
}

IOManager::FdContext* IOManager::getFdContext(int fd, bool auto_create) {
  if (fd < 0 || fd > kMaxTrackedFd) {
    errno = EINVAL;
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(fd_contexts_mutex_);
  if (static_cast<size_t>(fd) >= fd_contexts_.size()) {
    if (!auto_create) {
      return nullptr;
    }
    fd_contexts_.resize(static_cast<size_t>(fd) + 1);
  }

  auto& context = fd_contexts_[static_cast<size_t>(fd)];
  if (!context && auto_create) {
    context = std::make_unique<FdContext>(fd);
  }
  return context.get();
}

bool IOManager::updateEpoll(FdContext& context, EventMask events) {
  epoll_event event{};
  event.events = toEpollEvents(events);
  event.data.ptr = &context;

  const int operation = events == NONE ? EPOLL_CTL_DEL
                                       : (context.events == NONE
                                              ? EPOLL_CTL_ADD
                                              : EPOLL_CTL_MOD);
  epoll_event* event_ptr = operation == EPOLL_CTL_DEL ? nullptr : &event;
  return ::epoll_ctl(epoll_fd_, operation, context.fd, event_ptr) == 0;
}

void IOManager::trigger(EventContext&& context) {
  if (context.cb) {
    schedule(std::move(context.cb));
  } else if (context.fiber) {
    schedule(std::move(context.fiber));
  }
}

}  // namespace droplet
