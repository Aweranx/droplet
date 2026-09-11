#include "droplet/iomanager/iomanager.h"

#include <sys/epoll.h>

#include <cerrno>
#include <utility>

namespace droplet {

namespace {

constexpr IOManager::EventMask kAllEvents =
    IOManager::READ | IOManager::WRITE;

}  // namespace

int IOManager::addEvent(int fd, EventMask event, std::function<void()> cb) {
  if (!validEvent(event)) {
    errno = EINVAL;
    return -1;
  }

  EventContext handler;
  if (cb) {
    handler.cb = std::move(cb);
  } else {
    Fiber::Ptr current = Fiber::GetThis();
    if (!current || current->getStackSize() == 0) {
      errno = EPERM;
      return -1;
    }
    handler.fiber = std::move(current);
  }

  FdContext* context = getFdContext(fd, true);
  if (!context) {
    return -1;
  }

  {
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->events & event) {
      errno = EEXIST;
      return -1;
    }

    const EventMask new_events = context->events | event;
    if (!updateEpoll(*context, new_events)) {
      return -1;
    }

    if (event == READ) {
      context->read = std::move(handler);
    } else {
      context->write = std::move(handler);
    }
    context->events = new_events;
    pending_event_count_.fetch_add(1, std::memory_order_release);
  }

  // epoll_ctl 可能发生在另一个线程正在 epoll_wait 时，显式踹醒可以让
  // 新注册的定时器/就绪 fd 尽快被观察到。
  tickle();
  return 0;
}

bool IOManager::delEvent(int fd, EventMask event) {
  if (event == NONE || (event & ~kAllEvents)) {
    errno = EINVAL;
    return false;
  }

  FdContext* context = getFdContext(fd, false);
  if (!context) {
    errno = ENOENT;
    return false;
  }

  size_t removed = 0;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    const EventMask actual = context->events & event;
    if (actual == NONE) {
      errno = ENOENT;
      return false;
    }
    const EventMask new_events = context->events & ~event;
    if (!updateEpoll(*context, new_events)) {
      return false;
    }

    if (actual & READ) {
      context->read.clear();
      ++removed;
    }
    if (actual & WRITE) {
      context->write.clear();
      ++removed;
    }
    context->events = new_events;
  }

  pending_event_count_.fetch_sub(removed, std::memory_order_release);
  tickle();
  return true;
}

bool IOManager::cancelEvent(int fd, EventMask event) {
  if (event == NONE || (event & ~kAllEvents)) {
    errno = EINVAL;
    return false;
  }

  FdContext* context = getFdContext(fd, false);
  if (!context) {
    errno = ENOENT;
    return false;
  }

  EventContext read_handler;
  EventContext write_handler;
  size_t removed = 0;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    const EventMask actual = context->events & event;
    if (actual == NONE) {
      errno = ENOENT;
      return false;
    }
    const EventMask new_events = context->events & ~event;
    if (!updateEpoll(*context, new_events)) {
      return false;
    }

    if (actual & READ) {
      read_handler = std::move(context->read);
      ++removed;
    }
    if (actual & WRITE) {
      write_handler = std::move(context->write);
      ++removed;
    }
    context->events = new_events;
  }

  pending_event_count_.fetch_sub(removed, std::memory_order_release);
  trigger(std::move(read_handler));
  trigger(std::move(write_handler));
  tickle();
  return true;
}

bool IOManager::cancelAll(int fd) { return cancelEvent(fd, kAllEvents); }

bool IOManager::processEvent(const void* epoll_data,
                             uint32_t epoll_events) {
  auto* context = const_cast<FdContext*>(
      static_cast<const FdContext*>(epoll_data));
  if (!context) {
    return false;
  }

  EventMask ready = NONE;
  if (epoll_events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    epoll_events |= EPOLLIN | EPOLLOUT;
  }
  if (epoll_events & EPOLLIN) {
    ready |= READ;
  }
  if (epoll_events & EPOLLOUT) {
    ready |= WRITE;
  }

  EventContext read_handler;
  EventContext write_handler;
  size_t removed = 0;
  {
    std::lock_guard<std::mutex> lock(context->mutex);
    ready &= context->events;
    if (ready == NONE) {
      return false;
    }

    const EventMask new_events = context->events & ~ready;
    // 就绪事件是一次性消费的；重新监听由回调/Fiber 自己决定。
    // 即使 fd 已被并发关闭，也要清理本地状态，避免 pending 数量泄漏。
    (void)updateEpoll(*context, new_events);
    if (ready & READ) {
      read_handler = std::move(context->read);
      ++removed;
    }
    if (ready & WRITE) {
      write_handler = std::move(context->write);
      ++removed;
    }
    context->events = new_events;
  }

  pending_event_count_.fetch_sub(removed, std::memory_order_release);
  trigger(std::move(read_handler));
  trigger(std::move(write_handler));
  return true;
}

}  // namespace droplet
