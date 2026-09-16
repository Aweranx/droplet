#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace droplet {

/**
 * @brief 网络地址的抽象封装。
 *
 * Address 只负责保存 sockaddr 及其长度；Socket 负责使用地址执行
 * bind/connect/send/recv。IPv4、IPv6 和 Unix domain socket 由派生类实现。
 */
class DROPLET_API Address {
 public:
  using Ptr = std::shared_ptr<Address>;
  using ptr = Ptr;

  static Ptr Create(const sockaddr* address, socklen_t length);
  // 解析主机名或 IP，返回所有符合条件的地址。
  static bool Lookup(std::vector<Ptr>& result, const std::string& host,
                     int family = AF_INET, int type = 0, int protocol = 0);
  static Ptr LookupAny(const std::string& host, int family = AF_INET,
                       int type = 0, int protocol = 0);

  static std::shared_ptr<class IPAddress> LookupAnyIPAddress(
      const std::string& host, int family = AF_INET, int type = 0,
      int protocol = 0);

  // 获取本机所有网卡地址。
  static bool GetInterfaceAddresses(
      std::multimap<std::string, std::pair<Ptr, u32>>& result,
      int family = AF_INET);
  static bool GetInterfaceAddresses(
      std::vector<std::pair<Ptr, u32>>& result, const std::string& iface,
      int family = AF_INET);

  virtual ~Address() = default;

  [[nodiscard]] int getFamily() const noexcept { return family_; }
  [[nodiscard]] virtual const sockaddr* getAddr() const noexcept = 0;
  [[nodiscard]] virtual sockaddr* getAddr() noexcept = 0;
  [[nodiscard]] virtual socklen_t getAddrLen() const noexcept = 0;

  virtual std::ostream& insert(std::ostream& os) const = 0;
  [[nodiscard]] std::string toString() const;

  bool operator<(const Address& rhs) const noexcept;
  bool operator==(const Address& rhs) const noexcept;
  bool operator!=(const Address& rhs) const noexcept { return !(*this == rhs); }

 protected:
  explicit Address(int family) : family_(family) {}

 private:
  int family_;
};

std::ostream& operator<<(std::ostream& os, const Address& address);

/** @brief IP 地址抽象基类。 */
class DROPLET_API IPAddress : public Address {
 public:
  using Ptr = std::shared_ptr<IPAddress>;
  using ptr = Ptr;

  static Ptr Create(const char* address, u16 port = 0);

  virtual Ptr broadcastAddress(u32 prefix_length) const = 0;
  virtual Ptr networkAddress(u32 prefix_length) const = 0;

  virtual Ptr subnetMask(u32 prefix_length) const = 0;

  [[nodiscard]] virtual u32 getPort() const noexcept = 0;
  virtual void setPort(u16 port) noexcept = 0;

 protected:
  explicit IPAddress(int family) : Address(family) {}
};

/** @brief IPv4 地址。 */
class DROPLET_API IPv4Address final : public IPAddress {
 public:
  using Ptr = std::shared_ptr<IPv4Address>;
  using ptr = Ptr;

  static Ptr Create(const char* address, u16 port = 0);

  explicit IPv4Address(const sockaddr_in& address);
  explicit IPv4Address(u32 address = INADDR_ANY, u16 port = 0);

  [[nodiscard]] const sockaddr* getAddr() const noexcept override;
  [[nodiscard]] sockaddr* getAddr() noexcept override;
  [[nodiscard]] socklen_t getAddrLen() const noexcept override;
  std::ostream& insert(std::ostream& os) const override;

  [[nodiscard]] IPAddress::Ptr broadcastAddress(
      u32 prefix_length) const override;
  [[nodiscard]] IPAddress::Ptr networkAddress(
      u32 prefix_length) const override;
  [[nodiscard]] IPAddress::Ptr subnetMask(
      u32 prefix_length) const override;
  [[nodiscard]] u32 getPort() const noexcept override;
  void setPort(u16 port) noexcept override;

 private:
  sockaddr_in address_{};
};

/** @brief IPv6 地址。 */
class DROPLET_API IPv6Address final : public IPAddress {
 public:
  using Ptr = std::shared_ptr<IPv6Address>;
  using ptr = Ptr;

  static Ptr Create(const char* address, u16 port = 0);

  IPv6Address();
  explicit IPv6Address(const sockaddr_in6& address);
  IPv6Address(const u8 address[16], u16 port = 0);

  [[nodiscard]] const sockaddr* getAddr() const noexcept override;
  [[nodiscard]] sockaddr* getAddr() noexcept override;
  [[nodiscard]] socklen_t getAddrLen() const noexcept override;
  std::ostream& insert(std::ostream& os) const override;

  [[nodiscard]] IPAddress::Ptr broadcastAddress(
      u32 prefix_length) const override;
  [[nodiscard]] IPAddress::Ptr networkAddress(
      u32 prefix_length) const override;
  [[nodiscard]] IPAddress::Ptr subnetMask(
      u32 prefix_length) const override;
  [[nodiscard]] u32 getPort() const noexcept override;
  void setPort(u16 port) noexcept override;

 private:
  sockaddr_in6 address_{};
};

/** @brief Unix domain socket 地址。 */
class DROPLET_API UnixAddress final : public Address {
 public:
  using Ptr = std::shared_ptr<UnixAddress>;

  UnixAddress();
  explicit UnixAddress(std::string path);
  UnixAddress(const sockaddr_un& address, socklen_t length);

  [[nodiscard]] const sockaddr* getAddr() const noexcept override;
  [[nodiscard]] sockaddr* getAddr() noexcept override;
  [[nodiscard]] socklen_t getAddrLen() const noexcept override;
  std::ostream& insert(std::ostream& os) const override;

  void setAddrLen(socklen_t length) noexcept { length_ = length; }
  [[nodiscard]] const std::string& getPath() const noexcept { return path_; }

 private:
  sockaddr_un address_{};
  socklen_t length_{sizeof(sockaddr_un)};
  std::string path_;
};

/** @brief 无法识别协议族时保存原始 sockaddr 的地址类型。 */
class DROPLET_API UnknownAddress final : public Address {
 public:
  using Ptr = std::shared_ptr<UnknownAddress>;

  UnknownAddress(const sockaddr& address, socklen_t length);
  explicit UnknownAddress(int family);
  explicit UnknownAddress(const sockaddr& address)
      : UnknownAddress(address, sizeof(sockaddr)) {}

  [[nodiscard]] const sockaddr* getAddr() const noexcept override;
  [[nodiscard]] sockaddr* getAddr() noexcept override;
  [[nodiscard]] socklen_t getAddrLen() const noexcept override;
  std::ostream& insert(std::ostream& os) const override;

 private:
  sockaddr_storage address_{};
  socklen_t length_{};
};

}  // namespace droplet
