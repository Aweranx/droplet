#include "droplet/hook/hook.h"
#include <droplet/types.h>

#include <droplet/socket/fd_manager.h>
#include <droplet/fiber/fiber.h>
#include <droplet/scheduler/iomanager.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <utility>

extern "C" {

sleep_fun sleep_f = nullptr;
usleep_fun usleep_f = nullptr;
nanosleep_fun nanosleep_f = nullptr;
socket_fun socket_f = nullptr;
socketpair_fun socketpair_f = nullptr;
connect_fun connect_f = nullptr;
accept_fun accept_f = nullptr;
accept4_fun accept4_f = nullptr;
read_fun read_f = nullptr;
readv_fun readv_f = nullptr;
recv_fun recv_f = nullptr;
recvfrom_fun recvfrom_f = nullptr;
recvmsg_fun recvmsg_f = nullptr;
write_fun write_f = nullptr;
writev_fun writev_f = nullptr;
send_fun send_f = nullptr;
sendto_fun sendto_f = nullptr;
sendmsg_fun sendmsg_f = nullptr;
close_fun close_f = nullptr;
fcntl_fun fcntl_f = nullptr;
ioctl_fun ioctl_f = nullptr;
getsockopt_fun getsockopt_f = nullptr;
setsockopt_fun setsockopt_f = nullptr;

}  // extern "C"

namespace droplet {

namespace {

thread_local bool t_hook_enabled = false;
std::once_flag g_init_once;
std::atomic<u64> g_connect_timeout{5000};

template <class Function>
Function Resolve(const char* name) noexcept {
  return reinterpret_cast<Function>(::dlsym(RTLD_NEXT, name));
}

void InitializeOriginalFunctions() noexcept {
  sleep_f = Resolve<sleep_fun>("sleep");
  usleep_f = Resolve<usleep_fun>("usleep");
  nanosleep_f = Resolve<nanosleep_fun>("nanosleep");
  socket_f = Resolve<socket_fun>("socket");
  socketpair_f = Resolve<socketpair_fun>("socketpair");
  connect_f = Resolve<connect_fun>("connect");
  accept_f = Resolve<accept_fun>("accept");
  accept4_f = Resolve<accept4_fun>("accept4");
  read_f = Resolve<read_fun>("read");
  readv_f = Resolve<readv_fun>("readv");
  recv_f = Resolve<recv_fun>("recv");
  recvfrom_f = Resolve<recvfrom_fun>("recvfrom");
  recvmsg_f = Resolve<recvmsg_fun>("recvmsg");
  write_f = Resolve<write_fun>("write");
  writev_f = Resolve<writev_fun>("writev");
  send_f = Resolve<send_fun>("send");
  sendto_f = Resolve<sendto_fun>("sendto");
  sendmsg_f = Resolve<sendmsg_fun>("sendmsg");
  close_f = Resolve<close_fun>("close");
  fcntl_f = Resolve<fcntl_fun>("fcntl");
  ioctl_f = Resolve<ioctl_fun>("ioctl");
  getsockopt_f = Resolve<getsockopt_fun>("getsockopt");
  setsockopt_f = Resolve<setsockopt_fun>("setsockopt");
}

void EnsureOriginalFunctions() noexcept {
  std::call_once(g_init_once, InitializeOriginalFunctions);
}

struct TimerInfo {
  std::atomic<int> cancelled{0};
};

template <class OriginFunction, class... Args>
ssize_t DoIo(int fd, OriginFunction function, const char* /*name*/,
             IOManager::EventMask event, int timeout_option, Args&&... args) {
  EnsureOriginalFunctions();
  if (!function) {
    errno = ENOSYS;
    return -1;
  }
  if (!t_hook_enabled) {
    return function(fd, std::forward<Args>(args)...);
  }

  // A socket may have been created before entering an IOManager thread.  The
  // first hooked I/O lazily creates its context and applies kernel O_NONBLOCK.
  FdCtx::Ptr context = FdMgr::GetInstance().get(fd, true);
  if (!context || context->isClose() || !context->isSocket() ||
      context->getUserNonblock()) {
    if (context && context->isClose()) {
      errno = EBADF;
      return -1;
    }
    return function(fd, std::forward<Args>(args)...);
  }

  const u64 timeout = context->getTimeout(timeout_option);
  auto timer_info = std::make_shared<TimerInfo>();

  while (true) {
    if (context->isClose()) {
      errno = EBADF;
      return -1;
    }
    ssize_t result = function(fd, std::forward<Args>(args)...);
    while (result == -1 && errno == EINTR) {
      result = function(fd, std::forward<Args>(args)...);
    }
    if (result != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      return result;
    }

    IOManager* iom = IOManager::GetThis();
    Fiber::Ptr current = Fiber::GetThis();
    if (!iom || !current || current->getStackSize() == 0) {
      return result;
    }

    TimerManager::TimerPtr timer;
    std::weak_ptr<TimerInfo> weak_info = timer_info;
    if (timeout != UINT64_MAX && timeout != 0) {
      std::weak_ptr<void> condition = timer_info;
      timer = iom->addConditionTimer(
          timeout,
          [weak_info, fd, event, iom] {
            auto info = weak_info.lock();
            if (!info) {
              return;
            }
            int expected = 0;
            if (!info->cancelled.compare_exchange_strong(
                    expected, ETIMEDOUT, std::memory_order_acq_rel)) {
              return;
            }
            (void)iom->cancelEvent(fd, event);
          },
          std::move(condition));
    }

    if (iom->addEvent(fd, event) != 0) {
      if (timer) {
        (void)timer->cancel();
      }
      return -1;
    }

    Fiber::yield_to_hold();
    if (timer) {
      (void)timer->cancel();
    }
    const int cancellation =
        timer_info->cancelled.load(std::memory_order_acquire);
    if (cancellation != 0) {
      errno = cancellation;
      return -1;
    }
  }
}

}  // namespace

bool isHookEnabled() noexcept { return t_hook_enabled; }

void setHookEnabled(bool enabled) noexcept { t_hook_enabled = enabled; }

// The function-pointer declarations have C linkage and live in the global
// namespace (matching the interposed symbols).  Bring them into droplet's
// scope for the implementation's qualified calls below.
using ::accept_f;
using ::accept4_f;
using ::close_f;
using ::connect_f;
using ::fcntl_f;
using ::getsockopt_f;
using ::ioctl_f;
using ::nanosleep_f;
using ::read_f;
using ::readv_f;
using ::recv_f;
using ::recvfrom_f;
using ::recvmsg_f;
using ::send_f;
using ::sendmsg_f;
using ::sendto_f;
using ::setsockopt_f;
using ::sleep_f;
using ::socket_f;
using ::socketpair_f;
using ::usleep_f;
using ::write_f;
using ::writev_f;

}  // namespace droplet

extern "C" {

unsigned int sleep(unsigned int seconds) {
  droplet::EnsureOriginalFunctions();
  if (!droplet::isHookEnabled()) {
    return droplet::sleep_f(seconds);
  }
  auto* iom = droplet::IOManager::GetThis();
  auto fiber = droplet::Fiber::GetThis();
  if (!iom || !fiber || fiber->getStackSize() == 0) {
    return droplet::sleep_f(seconds);
  }
  (void)iom->addTimer(static_cast<u64>(seconds) * 1000,
                      [iom, fiber] { iom->schedule(fiber); });
  droplet::Fiber::yield_to_hold();
  return 0;
}

int usleep(useconds_t usec) {
  droplet::EnsureOriginalFunctions();
  if (!droplet::isHookEnabled()) {
    return droplet::usleep_f(usec);
  }
  auto* iom = droplet::IOManager::GetThis();
  auto fiber = droplet::Fiber::GetThis();
  if (!iom || !fiber || fiber->getStackSize() == 0 || usec == 0) {
    return droplet::usleep_f(usec);
  }
  const u64 milliseconds = (static_cast<u64>(usec) + 999) / 1000;
  (void)iom->addTimer(milliseconds, [iom, fiber] { iom->schedule(fiber); });
  droplet::Fiber::yield_to_hold();
  return 0;
}

int nanosleep(const timespec* request, timespec* remainder) {
  droplet::EnsureOriginalFunctions();
  if (!request || !droplet::isHookEnabled()) {
    return droplet::nanosleep_f(request, remainder);
  }
  auto* iom = droplet::IOManager::GetThis();
  auto fiber = droplet::Fiber::GetThis();
  if (!iom || !fiber || fiber->getStackSize() == 0) {
    return droplet::nanosleep_f(request, remainder);
  }
  const u64 milliseconds =
      static_cast<u64>(request->tv_sec) * 1000 +
      (static_cast<u64>(request->tv_nsec) + 999999) / 1000000;
  (void)iom->addTimer(milliseconds, [iom, fiber] { iom->schedule(fiber); });
  droplet::Fiber::yield_to_hold();
  if (remainder) {
    *remainder = {};
  }
  return 0;
}

int socket(int domain, int type, int protocol) {
  droplet::EnsureOriginalFunctions();
  const int fd = droplet::socket_f(domain, type, protocol);
  if (fd >= 0 && droplet::isHookEnabled()) {
    if (auto context = droplet::FdMgr::GetInstance().get(fd, true)) {
#ifdef SOCK_NONBLOCK
      context->setUserNonblock((type & SOCK_NONBLOCK) != 0);
#endif
    }
  }
  return fd;
}

int socketpair(int domain, int type, int protocol, int sockets[2]) {
  droplet::EnsureOriginalFunctions();
  const int result = droplet::socketpair_f(domain, type, protocol, sockets);
  if (result == 0 && droplet::isHookEnabled() && sockets) {
    auto first = droplet::FdMgr::GetInstance().get(sockets[0], true);
    auto second = droplet::FdMgr::GetInstance().get(sockets[1], true);
#ifdef SOCK_NONBLOCK
    if (first) {
      first->setUserNonblock((type & SOCK_NONBLOCK) != 0);
    }
    if (second) {
      second->setUserNonblock((type & SOCK_NONBLOCK) != 0);
    }
#endif
  }
  return result;
}

int connect_with_timeout(int fd, const sockaddr* address, socklen_t length,
                         u64 timeout_ms) {
  droplet::EnsureOriginalFunctions();
  if (!droplet::isHookEnabled()) {
    return droplet::connect_f(fd, address, length);
  }

  auto context = droplet::FdMgr::GetInstance().get(fd, true);
  if (!context || context->isClose() || !context->isSocket() ||
      context->getUserNonblock()) {
    if (context && context->isClose()) {
      errno = EBADF;
      return -1;
    }
    return droplet::connect_f(fd, address, length);
  }

  int result = droplet::connect_f(fd, address, length);
  if (result == 0 || (result == -1 && errno != EINPROGRESS)) {
    return result;
  }

  auto* iom = droplet::IOManager::GetThis();
  auto fiber = droplet::Fiber::GetThis();
  if (!iom || !fiber || fiber->getStackSize() == 0) {
    return result;
  }

  auto timer_info = std::make_shared<droplet::TimerInfo>();
  droplet::TimerManager::TimerPtr timer;
  std::weak_ptr<droplet::TimerInfo> weak_info = timer_info;
  if (timeout_ms != UINT64_MAX) {
    std::weak_ptr<void> condition = timer_info;
    timer = iom->addConditionTimer(
        timeout_ms,
        [weak_info, fd, iom] {
          auto info = weak_info.lock();
          if (!info) {
            return;
          }
          int expected = 0;
          if (!info->cancelled.compare_exchange_strong(
                  expected, ETIMEDOUT, std::memory_order_acq_rel)) {
            return;
          }
          (void)iom->cancelEvent(fd, droplet::IOManager::WRITE);
        },
        std::move(condition));
  }

  if (iom->addEvent(fd, droplet::IOManager::WRITE) != 0) {
    if (timer) {
      (void)timer->cancel();
    }
    return -1;
  }

  droplet::Fiber::yield_to_hold();
  if (timer) {
    (void)timer->cancel();
  }
  const int cancellation =
      timer_info->cancelled.load(std::memory_order_acquire);
  if (cancellation != 0) {
    errno = cancellation;
    return -1;
  }
  if (context->isClose()) {
    errno = EBADF;
    return -1;
  }

  int error = 0;
  socklen_t error_length = sizeof(error);
  if (droplet::getsockopt_f(fd, SOL_SOCKET, SO_ERROR, &error,
                            &error_length) != 0) {
    return -1;
  }
  if (error != 0) {
    errno = error;
    return -1;
  }
  return 0;
}

int connect(int fd, const sockaddr* address, socklen_t length) {
  return connect_with_timeout(fd, address, length,
                              droplet::g_connect_timeout.load());
}

int accept(int fd, sockaddr* address, socklen_t* length) {
  droplet::EnsureOriginalFunctions();
  const int result = static_cast<int>(droplet::DoIo(
      fd, droplet::accept_f, "accept", droplet::IOManager::READ,
      SO_RCVTIMEO, address, length));
  if (result >= 0 && droplet::isHookEnabled()) {
    (void)droplet::FdMgr::GetInstance().get(result, true);
  }
  return result;
}

int accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
  droplet::EnsureOriginalFunctions();
  if (!droplet::accept4_f) {
    errno = ENOSYS;
    return -1;
  }
  const int result = static_cast<int>(droplet::DoIo(
      fd, droplet::accept4_f, "accept4", droplet::IOManager::READ,
      SO_RCVTIMEO, address, length, flags));
  if (result >= 0 && droplet::isHookEnabled()) {
    if (auto context = droplet::FdMgr::GetInstance().get(result, true)) {
#ifdef SOCK_NONBLOCK
      context->setUserNonblock((flags & SOCK_NONBLOCK) != 0);
#endif
    }
  }
  return result;
}

ssize_t read(int fd, void* buffer, size_t count) {
  droplet::EnsureOriginalFunctions();
  return droplet::DoIo(fd, droplet::read_f, "read", droplet::IOManager::READ,
                       SO_RCVTIMEO, buffer, count);
}

ssize_t readv(int fd, const iovec* buffers, int count) {
  droplet::EnsureOriginalFunctions();
  return droplet::DoIo(fd, droplet::readv_f, "readv",
                       droplet::IOManager::READ, SO_RCVTIMEO, buffers, count);
}

ssize_t recv(int fd, void* buffer, size_t length, int flags) {
  droplet::EnsureOriginalFunctions();
  if (flags & MSG_DONTWAIT) {
    return droplet::recv_f(fd, buffer, length, flags);
  }
  return droplet::DoIo(fd, droplet::recv_f, "recv", droplet::IOManager::READ,
                       SO_RCVTIMEO, buffer, length, flags);
}

ssize_t recvfrom(int fd, void* buffer, size_t length, int flags,
                 sockaddr* address, socklen_t* address_length) {
  droplet::EnsureOriginalFunctions();
  if (flags & MSG_DONTWAIT) {
    return droplet::recvfrom_f(fd, buffer, length, flags, address,
                               address_length);
  }
  return droplet::DoIo(fd, droplet::recvfrom_f, "recvfrom",
                       droplet::IOManager::READ, SO_RCVTIMEO, buffer, length,
                       flags, address, address_length);
}

ssize_t recvmsg(int fd, msghdr* message, int flags) {
  droplet::EnsureOriginalFunctions();
  if (flags & MSG_DONTWAIT) {
    return droplet::recvmsg_f(fd, message, flags);
  }
  return droplet::DoIo(fd, droplet::recvmsg_f, "recvmsg",
                       droplet::IOManager::READ, SO_RCVTIMEO, message, flags);
}

ssize_t write(int fd, const void* buffer, size_t count) {
  droplet::EnsureOriginalFunctions();
  return droplet::DoIo(fd, droplet::write_f, "write",
                       droplet::IOManager::WRITE, SO_SNDTIMEO, buffer, count);
}

ssize_t writev(int fd, const iovec* buffers, int count) {
  droplet::EnsureOriginalFunctions();
  return droplet::DoIo(fd, droplet::writev_f, "writev",
                       droplet::IOManager::WRITE, SO_SNDTIMEO, buffers, count);
}

ssize_t send(int fd, const void* buffer, size_t length, int flags) {
  droplet::EnsureOriginalFunctions();
  if (flags & MSG_DONTWAIT) {
    return droplet::send_f(fd, buffer, length, flags);
  }
  return droplet::DoIo(fd, droplet::send_f, "send", droplet::IOManager::WRITE,
                       SO_SNDTIMEO, buffer, length, flags);
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const sockaddr* address, socklen_t address_length) {
  droplet::EnsureOriginalFunctions();
  if (flags & MSG_DONTWAIT) {
    return droplet::sendto_f(fd, buffer, length, flags, address,
                             address_length);
  }
  return droplet::DoIo(fd, droplet::sendto_f, "sendto",
                       droplet::IOManager::WRITE, SO_SNDTIMEO, buffer, length,
                       flags, address, address_length);
}

ssize_t sendmsg(int fd, const msghdr* message, int flags) {
  droplet::EnsureOriginalFunctions();
  if (flags & MSG_DONTWAIT) {
    return droplet::sendmsg_f(fd, message, flags);
  }
  return droplet::DoIo(fd, droplet::sendmsg_f, "sendmsg",
                       droplet::IOManager::WRITE, SO_SNDTIMEO, message, flags);
}

int close(int fd) {
  droplet::EnsureOriginalFunctions();
  if (auto context = droplet::FdMgr::GetInstance().get(fd)) {
    // Mark first so a Fiber awakened by cancelAll cannot issue another I/O
    // while this close is racing on a different worker thread.
    context->markClosed();
    if (auto* iom = droplet::IOManager::GetThis()) {
      (void)iom->cancelAll(fd);
    }
    droplet::FdMgr::GetInstance().del(fd);
  }
  return droplet::close_f(fd);
}

int fcntl(int fd, int command, ...) {
  droplet::EnsureOriginalFunctions();
  va_list args;
  va_start(args, command);

  auto context = droplet::FdMgr::GetInstance().get(fd);
  switch (command) {
    case F_SETFL: {
      const int value = va_arg(args, int);
      va_end(args);
      if (context && context->isSocket() && !context->isClose()) {
        context->setUserNonblock((value & O_NONBLOCK) != 0);
        int actual = value;
        if (context->getSysNonblock()) {
          actual |= O_NONBLOCK;
        }
        return droplet::fcntl_f(fd, command, actual);
      }
      return droplet::fcntl_f(fd, command, value);
    }
    case F_GETFL: {
      va_end(args);
      int value = droplet::fcntl_f(fd, command);
      if (value >= 0 && context && context->isSocket() &&
          !context->getUserNonblock()) {
        value &= ~O_NONBLOCK;
      }
      return value;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    case F_SETFD:
    case F_SETOWN:
    case F_SETSIG:
    case F_SETLEASE:
    case F_NOTIFY:
#ifdef F_SETPIPE_SZ
    case F_SETPIPE_SZ:
#endif
    {
      const int value = va_arg(args, int);
      va_end(args);
      return droplet::fcntl_f(fd, command, value);
    }
    case F_GETFD:
    case F_GETOWN:
    case F_GETSIG:
    case F_GETLEASE:
#ifdef F_GETPIPE_SZ
    case F_GETPIPE_SZ:
#endif
      va_end(args);
      return droplet::fcntl_f(fd, command);
    default: {
      va_end(args);
      return droplet::fcntl_f(fd, command);
    }
  }
}

int ioctl(int fd, unsigned long request, ...) {
  droplet::EnsureOriginalFunctions();
  va_list args;
  va_start(args, request);
  void* argument = va_arg(args, void*);
  va_end(args);

  const int result = droplet::ioctl_f(fd, request, argument);
  if (request == FIONBIO && argument) {
    if (auto context = droplet::FdMgr::GetInstance().get(fd)) {
      context->setUserNonblock(*static_cast<int*>(argument) != 0);
      if (context->isSocket() && context->getSysNonblock() && result == 0) {
        const int flags = droplet::fcntl_f(fd, F_GETFL);
        if (flags >= 0) {
          (void)droplet::fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
        }
      }
    }
  }
  return result;
}

int getsockopt(int fd, int level, int option, void* result,
               socklen_t* length) {
  droplet::EnsureOriginalFunctions();
  return droplet::getsockopt_f(fd, level, option, result, length);
}

int setsockopt(int fd, int level, int option, const void* value,
               socklen_t length) {
  droplet::EnsureOriginalFunctions();
  const int result = droplet::setsockopt_f(fd, level, option, value, length);
  if (result == 0 && level == SOL_SOCKET && value &&
      (option == SO_RCVTIMEO || option == SO_SNDTIMEO) &&
      length >= sizeof(timeval)) {
    if (auto context = droplet::FdMgr::GetInstance().get(fd)) {
      const auto* timeout = static_cast<const timeval*>(value);
      const u64 milliseconds =
          static_cast<u64>(timeout->tv_sec) * 1000 +
          static_cast<u64>(timeout->tv_usec + 999) / 1000;
      // A zero timeval means "no timeout"; any positive sub-millisecond
      // value still needs a finite wait, rounded up to one millisecond.
      const bool no_timeout = timeout->tv_sec == 0 && timeout->tv_usec == 0;
      context->setTimeout(option, no_timeout ? UINT64_MAX : milliseconds);
    }
  }
  return result;
}

}  // extern "C"
