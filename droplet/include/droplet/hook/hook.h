#pragma once

#include <droplet/export.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace droplet {

/// 当前线程是否启用阻塞 I/O 到 Fiber 的自动转换。
[[nodiscard]] DROPLET_API bool isHookEnabled() noexcept;
/// 设置当前线程的 hook 开关；Scheduler worker 默认会打开它。
DROPLET_API void setHookEnabled(bool enabled) noexcept;

// Sylar 风格的兼容命名。
[[nodiscard]] inline bool is_hook_enable() noexcept {
  return isHookEnabled();
}
inline void set_hook_enable(bool enabled) noexcept { setHookEnabled(enabled); }

}  // namespace droplet

extern "C" {

using sleep_fun = unsigned int (*)(unsigned int);
using usleep_fun = int (*)(useconds_t);
using nanosleep_fun = int (*)(const timespec*, timespec*);
using socket_fun = int (*)(int, int, int);
using socketpair_fun = int (*)(int, int, int, int[2]);
using connect_fun = int (*)(int, const sockaddr*, socklen_t);
using accept_fun = int (*)(int, sockaddr*, socklen_t*);
using accept4_fun = int (*)(int, sockaddr*, socklen_t*, int);
using read_fun = ssize_t (*)(int, void*, size_t);
using readv_fun = ssize_t (*)(int, const iovec*, int);
using recv_fun = ssize_t (*)(int, void*, size_t, int);
using recvfrom_fun = ssize_t (*)(int, void*, size_t, int, sockaddr*, socklen_t*);
using recvmsg_fun = ssize_t (*)(int, msghdr*, int);
using write_fun = ssize_t (*)(int, const void*, size_t);
using writev_fun = ssize_t (*)(int, const iovec*, int);
using send_fun = ssize_t (*)(int, const void*, size_t, int);
using sendto_fun = ssize_t (*)(int, const void*, size_t, int, const sockaddr*,
                               socklen_t);
using sendmsg_fun = ssize_t (*)(int, const msghdr*, int);
using close_fun = int (*)(int);
using fcntl_fun = int (*)(int, int, ...);
using ioctl_fun = int (*)(int, unsigned long, ...);
using getsockopt_fun = int (*)(int, int, int, void*, socklen_t*);
using setsockopt_fun = int (*)(int, int, int, const void*, socklen_t);

extern sleep_fun sleep_f;
extern usleep_fun usleep_f;
extern nanosleep_fun nanosleep_f;
extern socket_fun socket_f;
extern socketpair_fun socketpair_f;
extern connect_fun connect_f;
extern accept_fun accept_f;
extern accept4_fun accept4_f;
extern read_fun read_f;
extern readv_fun readv_f;
extern recv_fun recv_f;
extern recvfrom_fun recvfrom_f;
extern recvmsg_fun recvmsg_f;
extern write_fun write_f;
extern writev_fun writev_f;
extern send_fun send_f;
extern sendto_fun sendto_f;
extern sendmsg_fun sendmsg_f;
extern close_fun close_f;
extern fcntl_fun fcntl_f;
extern ioctl_fun ioctl_f;
extern getsockopt_fun getsockopt_f;
extern setsockopt_fun setsockopt_f;

int connect_with_timeout(int fd, const sockaddr* address, socklen_t length,
                         uint64_t timeout_ms);

}  // extern "C"
