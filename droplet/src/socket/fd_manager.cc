#include "droplet/socket/fd_manager.h"
#include <droplet/types.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>

namespace droplet {

namespace {

int RawFcntl(int fd, int command, long argument = 0) noexcept {
  return static_cast<int>(::syscall(SYS_fcntl, fd, command, argument));
}

}  // namespace

FdCtx::FdCtx(int fd) : fd_(fd) { (void)init(); }

bool FdCtx::init() noexcept {
  struct stat status {};
  if (::fstat(fd_, &status) != 0) {
    return false;
  }

  const bool is_socket = S_ISSOCK(status.st_mode);
  socket_.store(is_socket, std::memory_order_release);
  if (is_socket) {
    const int flags = RawFcntl(fd_, F_GETFL);
    if (flags >= 0) {
      const bool already_nonblock = (flags & O_NONBLOCK) != 0;
      if (already_nonblock || RawFcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0) {
        sys_nonblock_.store(true, std::memory_order_release);
      }
    }

    // A socket can be created/configured before entering an IOManager thread.
    // Read existing SO_*TIMEO values directly so the first hooked I/O honors
    // those settings even though the setsockopt hook was not active then.
    for (const int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
      timeval timeout{};
      socklen_t length = sizeof(timeout);
      if (::syscall(SYS_getsockopt, fd_, SOL_SOCKET, option, &timeout,
                    &length) == 0) {
        const bool no_timeout = timeout.tv_sec == 0 && timeout.tv_usec == 0;
        const u64 milliseconds =
            static_cast<u64>(timeout.tv_sec) * 1000 +
            static_cast<u64>(timeout.tv_usec + 999) / 1000;
        setTimeout(option, no_timeout ? UINT64_MAX : milliseconds);
      }
    }
  }

  initialized_.store(true, std::memory_order_release);
  return true;
}

void FdCtx::setTimeout(int option, u64 milliseconds) noexcept {
  if (option == SO_RCVTIMEO) {
    recv_timeout_.store(milliseconds, std::memory_order_release);
  } else if (option == SO_SNDTIMEO) {
    send_timeout_.store(milliseconds, std::memory_order_release);
  }
}

u64 FdCtx::getTimeout(int option) const noexcept {
  if (option == SO_RCVTIMEO) {
    return recv_timeout_.load(std::memory_order_acquire);
  }
  if (option == SO_SNDTIMEO) {
    return send_timeout_.load(std::memory_order_acquire);
  }
  return UINT64_MAX;
}

FdCtx::Ptr FdManager::get(int fd, bool auto_create) {
  if (fd < 0 || fd > kMaxTrackedFd) {
    errno = EINVAL;
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (static_cast<size_t>(fd) >= data_.size()) {
    if (!auto_create) {
      return nullptr;
    }
    data_.resize(static_cast<size_t>(fd) + 1);
  }
  auto& context = data_[static_cast<size_t>(fd)];
  if (!context && auto_create) {
    context = std::make_shared<FdCtx>(fd);
  }
  return context;
}

void FdManager::del(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (static_cast<size_t>(fd) >= data_.size()) {
    return;
  }
  if (data_[static_cast<size_t>(fd)]) {
    data_[static_cast<size_t>(fd)]->markClosed();
    data_[static_cast<size_t>(fd)].reset();
  }
}

}  // namespace droplet
