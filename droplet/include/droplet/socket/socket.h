#pragma once

#include <droplet/types.h>

#include <droplet/socket/address.h>
#include <droplet/export.h>
#include <droplet/utils/noncopyable.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace droplet {

class IOManager;

/**
 * @brief 面向 TCP/UDP/Unix socket 的 RAII 封装。
 *
 * 当 hook 在 IOManager 调度线程中启用时，底层 socket 会自动设置为内核
 * 非阻塞，并由 hook 把 EAGAIN 转化为 Fiber 等待；Socket 本身仍提供同步
 * 风格的 connect/accept/send/recv 接口。
 */
class DROPLET_API Socket : public std::enable_shared_from_this<Socket>,
                           public Noncopyable {
 public:
  using Ptr = std::shared_ptr<Socket>;
  using WeakPtr = std::weak_ptr<Socket>;

  enum Type : int {
    TCP = SOCK_STREAM,
    UDP = SOCK_DGRAM,
  };

  enum Family : int {
    IPv4 = AF_INET,
    IPv6 = AF_INET6,
    UNIX = AF_UNIX,
  };

  static Ptr CreateTCP(Address::Ptr address);
  static Ptr CreateUDP(Address::Ptr address);
  static Ptr CreateTCPSocket();
  static Ptr CreateUDPSocket();
  static Ptr CreateTCPSocket6();
  static Ptr CreateUDPSocket6();
  static Ptr CreateUnixTCPSocket();
  static Ptr CreateUnixUDPSocket();

  Socket(int family, int type, int protocol = 0);
  virtual ~Socket();

  [[nodiscard]] i64 getSendTimeout() const;
  void setSendTimeout(i64 milliseconds);
  [[nodiscard]] i64 getRecvTimeout() const;
  void setRecvTimeout(i64 milliseconds);

  bool getOption(int level, int option, void* result, socklen_t* length) const;
  template <class T>
  bool getOption(int level, int option, T& result) const {
    socklen_t length = sizeof(T);
    return getOption(level, option, &result, &length);
  }
  bool setOption(int level, int option, const void* result,
                 socklen_t length);
  template <class T>
  bool setOption(int level, int option, const T& value) {
    return setOption(level, option, &value, sizeof(T));
  }

  virtual Ptr accept();
  virtual bool bind(const Address::Ptr& address);
  virtual bool connect(const Address::Ptr& address,
                       u64 timeout_ms = UINT64_MAX);
  virtual bool reconnect(u64 timeout_ms = UINT64_MAX);
  virtual bool listen(int backlog = SOMAXCONN);
  virtual bool close();

  virtual ssize_t send(const void* buffer, size_t length, int flags = 0);
  virtual ssize_t send(const iovec* buffers, size_t length, int flags = 0);
  virtual ssize_t sendTo(const void* buffer, size_t length,
                         const Address::Ptr& to, int flags = 0);
  virtual ssize_t sendTo(const iovec* buffers, size_t length,
                         const Address::Ptr& to, int flags = 0);
  virtual ssize_t recv(void* buffer, size_t length, int flags = 0);
  virtual ssize_t recv(iovec* buffers, size_t length, int flags = 0);
  virtual ssize_t recvFrom(void* buffer, size_t length, Address::Ptr from,
                           int flags = 0);
  virtual ssize_t recvFrom(iovec* buffers, size_t length, Address::Ptr from,
                           int flags = 0);

  [[nodiscard]] Address::Ptr getRemoteAddress();
  [[nodiscard]] Address::Ptr getLocalAddress();
  [[nodiscard]] int getFamily() const noexcept { return family_; }
  [[nodiscard]] int getType() const noexcept { return type_; }
  [[nodiscard]] int getProtocol() const noexcept { return protocol_; }
  [[nodiscard]] bool isConnected() const noexcept { return connected_; }
  [[nodiscard]] bool isValid() const noexcept { return socket_ != -1; }
  [[nodiscard]] int getError() const;
  [[nodiscard]] int getSocket() const noexcept { return socket_; }

  virtual std::ostream& dump(std::ostream& os) const;
  [[nodiscard]] virtual std::string toString() const;

  bool cancelRead();
  bool cancelWrite();
  bool cancelAccept();
  bool cancelAll();

 protected:
  void initSock();
  void newSock();
  virtual bool init(int socket);

  int socket_{-1};
  int family_;
  int type_;
  int protocol_;
  bool connected_{false};
  Address::Ptr local_address_;
  Address::Ptr remote_address_;
};

std::ostream& operator<<(std::ostream& os, const Socket& socket);

}  // namespace droplet
