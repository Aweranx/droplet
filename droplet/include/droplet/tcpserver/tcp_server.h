#pragma once

#include <droplet/export.h>
#include <droplet/iomanager/iomanager.h>
#include <droplet/socket/address.h>
#include <droplet/socket/socket.h>
#include <droplet/utils/noncopyable.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace droplet {

/**
 * @brief 基于 Socket 和 IOManager 的通用 TCP 服务器。
 *
 * accept_worker 负责监听连接，io_worker 负责执行 handleClient；worker
 * 预留给派生服务器调度耗时业务。三个 IOManager 均由调用方管理生命周期，
 * 必须比 TcpServer 上的调度任务存活更久。
 */
class DROPLET_API TcpServer : public std::enable_shared_from_this<TcpServer>,
                              public Noncopyable {
 public:
  using Ptr = std::shared_ptr<TcpServer>;

  static constexpr uint64_t kDefaultRecvTimeoutMs = 120'000;

  explicit TcpServer(IOManager* worker = IOManager::GetThis(),
                     IOManager* io_worker = IOManager::GetThis(),
                     IOManager* accept_worker = IOManager::GetThis());
  virtual ~TcpServer();

  /** @brief 绑定并监听一个地址。 */
  virtual bool bind(const Address::Ptr& address);

  /**
   * @brief 以全有或全无的方式绑定多个地址。
   * @param[in] addresses 要监听的地址。
   * @param[out] failures 绑定或监听失败的地址。
   */
  virtual bool bind(const std::vector<Address::Ptr>& addresses,
                    std::vector<Address::Ptr>& failures);

  /** @brief 启动每个监听 socket 的 accept 协程；重复调用是幂等的。 */
  virtual bool start();

  /**
   * @brief 停止接收新连接并关闭监听 socket；重复调用是幂等的。
   *
   * 已经交给 handleClient 的连接由派生类决定何时关闭。
   */
  virtual void stop();

  [[nodiscard]] uint64_t getRecvTimeout() const noexcept {
    return recv_timeout_ms_.load(std::memory_order_acquire);
  }
  void setRecvTimeout(uint64_t milliseconds) noexcept {
    recv_timeout_ms_.store(milliseconds, std::memory_order_release);
  }

  [[nodiscard]] std::string getName() const;
  virtual void setName(std::string name);
  [[nodiscard]] std::string getType() const;

  [[nodiscard]] bool isStop() const noexcept {
    return stopped_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool isStopped() const noexcept { return isStop(); }

  [[nodiscard]] std::vector<Socket::Ptr> getSockets() const;
  [[nodiscard]] std::vector<Socket::Ptr> getSocks() const {
    return getSockets();
  }
  [[nodiscard]] virtual std::string toString(
      const std::string& prefix = "") const;

 protected:
  /** @brief 处理一个已建立的客户端连接，派生服务器在这里实现协议。 */
  virtual void handleClient(Socket::Ptr client);

  /** @brief 在 accept_worker 中循环接收一个监听 socket 上的连接。 */
  virtual void startAccept(Socket::Ptr listener);

  /** @brief 供 HTTP 等派生服务器设置服务类型。 */
  void setType(std::string type);

  [[nodiscard]] IOManager* getWorker() const noexcept { return worker_; }
  [[nodiscard]] IOManager* getIoWorker() const noexcept { return io_worker_; }
  [[nodiscard]] IOManager* getAcceptWorker() const noexcept {
    return accept_worker_;
  }

 private:
  void closeListeners(std::vector<Socket::Ptr> listeners);

  IOManager* worker_;
  IOManager* io_worker_;
  IOManager* accept_worker_;

  mutable std::mutex mutex_;
  std::vector<Socket::Ptr> listeners_;
  std::string name_{"droplet/0.1.0"};
  std::string type_{"tcp"};
  std::atomic<uint64_t> recv_timeout_ms_{kDefaultRecvTimeoutMs};
  std::atomic<bool> stopped_{true};
};

}  // namespace droplet
