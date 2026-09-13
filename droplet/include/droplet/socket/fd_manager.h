#pragma once

#include <droplet/export.h>
#include <droplet/utils/singleton.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace droplet {

/**
 * @brief 一个文件描述符的运行时状态。
 *
 * hook 需要把内核层的非阻塞 fd 映射回用户看到的阻塞语义，因此除了
 * socket 类型和关闭状态，还要记录用户是否主动设置 O_NONBLOCK 以及
 * SO_RCVTIMEO/SO_SNDTIMEO 超时。
 */
class DROPLET_API FdCtx final : public std::enable_shared_from_this<FdCtx> {
 public:
  using Ptr = std::shared_ptr<FdCtx>;

  explicit FdCtx(int fd);
  ~FdCtx() = default;

  FdCtx(const FdCtx&) = delete;
  FdCtx& operator=(const FdCtx&) = delete;

  [[nodiscard]] int getFd() const noexcept { return fd_; }
  [[nodiscard]] bool isInit() const noexcept {
    return initialized_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool isSocket() const noexcept {
    return socket_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool isClose() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }

  void setUserNonblock(bool value) noexcept {
    user_nonblock_.store(value, std::memory_order_release);
  }
  [[nodiscard]] bool getUserNonblock() const noexcept {
    return user_nonblock_.load(std::memory_order_acquire);
  }

  void setSysNonblock(bool value) noexcept {
    sys_nonblock_.store(value, std::memory_order_release);
  }
  [[nodiscard]] bool getSysNonblock() const noexcept {
    return sys_nonblock_.load(std::memory_order_acquire);
  }

  void setTimeout(int option, uint64_t milliseconds) noexcept;
  [[nodiscard]] uint64_t getTimeout(int option) const noexcept;

  void markClosed() noexcept {
    closed_.store(true, std::memory_order_release);
  }

 private:
  bool init() noexcept;

  int fd_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> socket_{false};
  std::atomic<bool> sys_nonblock_{false};
  std::atomic<bool> user_nonblock_{false};
  std::atomic<bool> closed_{false};
  std::atomic<uint64_t> recv_timeout_{UINT64_MAX};
  std::atomic<uint64_t> send_timeout_{UINT64_MAX};
};

/** @brief 进程级 fd 上下文表，供 hook 和 Socket 共享。 */
class DROPLET_API FdManager final : public Singleton<FdManager> {
  friend class Singleton<FdManager>;

 private:
  FdManager() = default;
  ~FdManager() = default;

 public:
  [[nodiscard]] FdCtx::Ptr get(int fd, bool auto_create = false);
  void del(int fd) noexcept;

 private:
  static constexpr int kMaxTrackedFd = 1 << 20;

  std::mutex mutex_;
  std::vector<FdCtx::Ptr> data_;
};

/// 兼容 Sylar 的单例访问方式：FdMgr::GetInstance().get(fd)。
using FdMgr = FdManager;

}  // namespace droplet
