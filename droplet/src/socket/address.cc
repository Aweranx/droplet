#include "droplet/socket/address.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace droplet {

namespace {

std::pair<std::string, std::string> SplitHostService(const std::string& host) {
  if (!host.empty() && host.front() == '[') {
    const auto close = host.find(']');
    if (close != std::string::npos) {
      std::string node = host.substr(1, close - 1);
      std::string service;
      if (close + 1 < host.size() && host[close + 1] == ':') {
        service = host.substr(close + 2);
      }
      return {std::move(node), std::move(service)};
    }
  }

  const auto first_colon = host.find(':');
  const auto last_colon = host.rfind(':');
  // 多个冒号通常意味着未加方括号的 IPv6 地址，不拆分端口。
  if (first_colon != std::string::npos && first_colon == last_colon) {
    return {host.substr(0, first_colon), host.substr(first_colon + 1)};
  }
  return {host, {}};
}

uint32_t CountMaskBits(const uint8_t* data, size_t length) noexcept {
  uint32_t result = 0;
  bool zero_seen = false;
  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = data[i];
    for (int bit = 7; bit >= 0; --bit) {
      if (byte & (uint8_t{1} << bit)) {
        if (zero_seen) {
          return result;
        }
        ++result;
      } else {
        zero_seen = true;
      }
    }
  }
  return result;
}

}  // namespace

Address::Ptr Address::Create(const sockaddr* address, socklen_t length) {
  if (!address || length < sizeof(sa_family_t)) {
    return nullptr;
  }

  switch (address->sa_family) {
    case AF_INET:
      if (length >= sizeof(sockaddr_in)) {
        return std::make_shared<IPv4Address>(
            *reinterpret_cast<const sockaddr_in*>(address));
      }
      break;
    case AF_INET6:
      if (length >= sizeof(sockaddr_in6)) {
        return std::make_shared<IPv6Address>(
            *reinterpret_cast<const sockaddr_in6*>(address));
      }
      break;
    case AF_UNIX:
      {
        sockaddr_un value{};
        std::memcpy(&value, address,
                    std::min<socklen_t>(length, sizeof(value)));
        return std::make_shared<UnixAddress>(value, length);
      }
    default:
      break;
  }
  return std::make_shared<UnknownAddress>(*address, length);
}

bool Address::Lookup(std::vector<Ptr>& result, const std::string& host,
                     int family, int type, int protocol) {
  auto [node, service] = SplitHostService(host);
  addrinfo hints{};
  hints.ai_family = family;
  hints.ai_socktype = type;
  hints.ai_protocol = protocol;
  // Keep the resolver behavior predictable across systems (and match
  // getaddrinfo's normal IPv4/IPv6 selection when the caller asks for it).
  hints.ai_flags = 0;

  addrinfo* results = nullptr;
  const int error = ::getaddrinfo(node.empty() ? nullptr : node.c_str(),
                                  service.empty() ? nullptr : service.c_str(),
                                  &hints, &results);
  if (error != 0) {
    return false;
  }

  for (addrinfo* it = results; it; it = it->ai_next) {
    if (it->ai_addr && it->ai_addrlen > 0) {
      if (auto address = Create(it->ai_addr,
                                static_cast<socklen_t>(it->ai_addrlen))) {
        result.push_back(std::move(address));
      }
    }
  }
  ::freeaddrinfo(results);
  return !result.empty();
}

Address::Ptr Address::LookupAny(const std::string& host, int family, int type,
                                int protocol) {
  std::vector<Ptr> result;
  return Lookup(result, host, family, type, protocol) ? result.front()
                                                       : nullptr;
}

std::shared_ptr<IPAddress> Address::LookupAnyIPAddress(const std::string& host,
                                                       int family, int type,
                                                       int protocol) {
  std::vector<Ptr> result;
  if (!Lookup(result, host, family, type, protocol)) {
    return nullptr;
  }
  for (const auto& address : result) {
    if (auto ip = std::dynamic_pointer_cast<IPAddress>(address)) {
      return ip;
    }
  }
  return nullptr;
}

bool Address::GetInterfaceAddresses(
    std::multimap<std::string, std::pair<Ptr, uint32_t>>& result, int family) {
  ifaddrs* interfaces = nullptr;
  if (::getifaddrs(&interfaces) != 0) {
    return false;
  }

  for (ifaddrs* it = interfaces; it; it = it->ifa_next) {
    if (!it->ifa_addr || !it->ifa_netmask) {
      continue;
    }
    const int actual_family = it->ifa_addr->sa_family;
    if (family != AF_UNSPEC && family != actual_family) {
      continue;
    }
    socklen_t length = 0;
    uint32_t prefix = 0;
    if (actual_family == AF_INET) {
      length = sizeof(sockaddr_in);
      const auto* mask = reinterpret_cast<const sockaddr_in*>(it->ifa_netmask);
      prefix = CountMaskBits(
          reinterpret_cast<const uint8_t*>(&mask->sin_addr.s_addr), 4);
    } else if (actual_family == AF_INET6) {
      length = sizeof(sockaddr_in6);
      const auto* mask = reinterpret_cast<const sockaddr_in6*>(it->ifa_netmask);
      prefix = CountMaskBits(mask->sin6_addr.s6_addr, 16);
    } else {
      continue;
    }
    if (auto address = Create(it->ifa_addr, length)) {
      result.emplace(it->ifa_name ? it->ifa_name : "",
                     std::make_pair(std::move(address), prefix));
    }
  }
  ::freeifaddrs(interfaces);
  return true;
}

bool Address::GetInterfaceAddresses(
    std::vector<std::pair<Ptr, uint32_t>>& result, const std::string& iface,
    int family) {
  std::multimap<std::string, std::pair<Ptr, uint32_t>> all;
  if (!GetInterfaceAddresses(all, family)) {
    return false;
  }
  const auto range = all.equal_range(iface);
  for (auto it = range.first; it != range.second; ++it) {
    result.push_back(it->second);
  }
  return !result.empty();
}

std::string Address::toString() const {
  std::ostringstream stream;
  insert(stream);
  return stream.str();
}

bool Address::operator<(const Address& rhs) const noexcept {
  if (family_ != rhs.family_) {
    return family_ < rhs.family_;
  }
  const socklen_t lhs_length = getAddrLen();
  const socklen_t rhs_length = rhs.getAddrLen();
  if (lhs_length != rhs_length) {
    return lhs_length < rhs_length;
  }
  return std::memcmp(getAddr(), rhs.getAddr(), lhs_length) < 0;
}

bool Address::operator==(const Address& rhs) const noexcept {
  return family_ == rhs.family_ && getAddrLen() == rhs.getAddrLen() &&
         std::memcmp(getAddr(), rhs.getAddr(), getAddrLen()) == 0;
}

std::ostream& operator<<(std::ostream& os, const Address& address) {
  return address.insert(os);
}

IPAddress::Ptr IPAddress::Create(const char* address, uint16_t port) {
  if (!address) {
    return nullptr;
  }
  if (auto ipv4 = IPv4Address::Create(address, port)) {
    return ipv4;
  }
  return IPv6Address::Create(address, port);
}

IPv4Address::Ptr IPv4Address::Create(const char* address, uint16_t port) {
  sockaddr_in value{};
  value.sin_family = AF_INET;
  value.sin_port = htons(port);
  if (::inet_pton(AF_INET, address, &value.sin_addr) != 1) {
    return nullptr;
  }
  return std::make_shared<IPv4Address>(value);
}

IPv4Address::IPv4Address(const sockaddr_in& address)
    : IPAddress(AF_INET), address_(address) {}

IPv4Address::IPv4Address(uint32_t address, uint16_t port) : IPAddress(AF_INET) {
  address_.sin_family = AF_INET;
  // The integer constructor follows the public Sylar convention (host-order
  // address); sockaddr_in stores it in network byte order.
  address_.sin_addr.s_addr = htonl(address);
  address_.sin_port = htons(port);
}

const sockaddr* IPv4Address::getAddr() const noexcept {
  return reinterpret_cast<const sockaddr*>(&address_);
}

sockaddr* IPv4Address::getAddr() noexcept {
  return reinterpret_cast<sockaddr*>(&address_);
}

socklen_t IPv4Address::getAddrLen() const noexcept {
  return sizeof(address_);
}

std::ostream& IPv4Address::insert(std::ostream& os) const {
  char buffer[INET_ADDRSTRLEN]{};
  if (!::inet_ntop(AF_INET, &address_.sin_addr, buffer, sizeof(buffer))) {
    return os << "<invalid-ipv4>";
  }
  return os << buffer << ':' << ntohs(address_.sin_port);
}

IPAddress::Ptr IPv4Address::broadcastAddress(uint32_t prefix_length) const {
  if (prefix_length > 32) {
    return nullptr;
  }
  const uint32_t mask = prefix_length == 0
                            ? 0
                            : 0xffffffffu << (32 - prefix_length);
  const uint32_t host = ntohl(address_.sin_addr.s_addr);
  return std::make_shared<IPv4Address>(host | ~mask, getPort());
}

IPAddress::Ptr IPv4Address::networkAddress(uint32_t prefix_length) const {
  if (prefix_length > 32) {
    return nullptr;
  }
  const uint32_t mask = prefix_length == 0
                            ? 0
                            : 0xffffffffu << (32 - prefix_length);
  const uint32_t host = ntohl(address_.sin_addr.s_addr);
  return std::make_shared<IPv4Address>(host & mask, getPort());
}

IPAddress::Ptr IPv4Address::subnetMask(uint32_t prefix_length) const {
  if (prefix_length > 32) {
    return nullptr;
  }
  const uint32_t mask = prefix_length == 0
                            ? 0
                            : 0xffffffffu << (32 - prefix_length);
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_addr.s_addr = htonl(mask);
  return std::make_shared<IPv4Address>(result);
}

uint32_t IPv4Address::getPort() const noexcept {
  return ntohs(address_.sin_port);
}

void IPv4Address::setPort(uint16_t port) noexcept {
  address_.sin_port = htons(port);
}

IPv6Address::Ptr IPv6Address::Create(const char* address, uint16_t port) {
  sockaddr_in6 value{};
  value.sin6_family = AF_INET6;
  value.sin6_port = htons(port);
  if (::inet_pton(AF_INET6, address, &value.sin6_addr) != 1) {
    return nullptr;
  }
  return std::make_shared<IPv6Address>(value);
}

IPv6Address::IPv6Address() : IPAddress(AF_INET6) {
  address_.sin6_family = AF_INET6;
}

IPv6Address::IPv6Address(const sockaddr_in6& address)
    : IPAddress(AF_INET6), address_(address) {}

IPv6Address::IPv6Address(const uint8_t address[16], uint16_t port)
    : IPAddress(AF_INET6) {
  address_.sin6_family = AF_INET6;
  address_.sin6_port = htons(port);
  std::memcpy(address_.sin6_addr.s6_addr, address, 16);
}

const sockaddr* IPv6Address::getAddr() const noexcept {
  return reinterpret_cast<const sockaddr*>(&address_);
}

sockaddr* IPv6Address::getAddr() noexcept {
  return reinterpret_cast<sockaddr*>(&address_);
}

socklen_t IPv6Address::getAddrLen() const noexcept {
  return sizeof(address_);
}

std::ostream& IPv6Address::insert(std::ostream& os) const {
  char buffer[INET6_ADDRSTRLEN]{};
  if (!::inet_ntop(AF_INET6, &address_.sin6_addr, buffer, sizeof(buffer))) {
    return os << "<invalid-ipv6>";
  }
  return os << '[' << buffer << ']' << ':' << ntohs(address_.sin6_port);
}

IPAddress::Ptr IPv6Address::broadcastAddress(uint32_t prefix_length) const {
  if (prefix_length > 128) {
    return nullptr;
  }
  auto value = address_.sin6_addr;
  for (size_t i = 0; i < 16; ++i) {
    const uint32_t bit = static_cast<uint32_t>(i * 8);
    if (bit + 8 <= prefix_length) {
      continue;
    }
    if (bit >= prefix_length) {
      value.s6_addr[i] = 0xff;
    } else {
      value.s6_addr[i] |= static_cast<uint8_t>(0xffu >> (prefix_length - bit));
    }
  }
  sockaddr_in6 result = address_;
  result.sin6_addr = value;
  return std::make_shared<IPv6Address>(result);
}

IPAddress::Ptr IPv6Address::networkAddress(uint32_t prefix_length) const {
  if (prefix_length > 128) {
    return nullptr;
  }
  auto value = address_.sin6_addr;
  for (size_t i = 0; i < 16; ++i) {
    const uint32_t bit = static_cast<uint32_t>(i * 8);
    if (bit + 8 <= prefix_length) {
      continue;
    }
    if (bit >= prefix_length) {
      value.s6_addr[i] = 0;
    } else {
      value.s6_addr[i] &= static_cast<uint8_t>(0xffu << (8 - (prefix_length - bit)));
    }
  }
  sockaddr_in6 result = address_;
  result.sin6_addr = value;
  return std::make_shared<IPv6Address>(result);
}

IPAddress::Ptr IPv6Address::subnetMask(uint32_t prefix_length) const {
  if (prefix_length > 128) {
    return nullptr;
  }
  std::array<uint8_t, 16> mask{};
  for (size_t i = 0; i < 16; ++i) {
    const uint32_t bit = static_cast<uint32_t>(i * 8);
    if (bit + 8 <= prefix_length) {
      mask[i] = 0xff;
    } else if (bit < prefix_length) {
      mask[i] = static_cast<uint8_t>(0xffu << (8 - (prefix_length - bit)));
    }
  }
  return std::make_shared<IPv6Address>(mask.data(), 0);
}

uint32_t IPv6Address::getPort() const noexcept {
  return ntohs(address_.sin6_port);
}

void IPv6Address::setPort(uint16_t port) noexcept {
  address_.sin6_port = htons(port);
}

UnixAddress::UnixAddress() : Address(AF_UNIX) {
  address_.sun_family = AF_UNIX;
  // Match sockaddr_un's full storage size for callers that fill the address
  // after construction (e.g. getpeername/getsockname).
  length_ = sizeof(address_);
}

UnixAddress::UnixAddress(std::string path) : Address(AF_UNIX), path_(path) {
  address_.sun_family = AF_UNIX;
  const bool abstract = !path_.empty() && path_.front() == '\0';
  if ((!abstract && path_.size() >= sizeof(address_.sun_path)) ||
      (abstract && path_.size() > sizeof(address_.sun_path))) {
    throw std::length_error("Unix socket path is too long");
  }
  std::memcpy(address_.sun_path, path_.data(), path_.size());
  if (abstract) {
    // Linux abstract namespace 不包含末尾的 NUL。
    length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                     path_.size());
  } else {
    address_.sun_path[path_.size()] = '\0';
    length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                     path_.size() + 1);
  }
}

UnixAddress::UnixAddress(const sockaddr_un& address, socklen_t length)
    : Address(AF_UNIX), address_(address),
      length_(std::min<socklen_t>(length, sizeof(address_))) {
  const size_t offset = offsetof(sockaddr_un, sun_path);
  const size_t available = length_ > offset ? length_ - offset : 0;
  size_t used = std::min(available, sizeof(address_.sun_path));
  if (used > 0 && address_.sun_path[0] != '\0' &&
      address_.sun_path[used - 1] == '\0') {
    --used;
  }
  path_.assign(address_.sun_path, address_.sun_path + used);
}

const sockaddr* UnixAddress::getAddr() const noexcept {
  return reinterpret_cast<const sockaddr*>(&address_);
}

sockaddr* UnixAddress::getAddr() noexcept {
  return reinterpret_cast<sockaddr*>(&address_);
}

socklen_t UnixAddress::getAddrLen() const noexcept { return length_; }

std::ostream& UnixAddress::insert(std::ostream& os) const {
  if (!path_.empty() && path_.front() == '\0') {
    return os << "\\0" << path_.substr(1);
  }
  return os << path_;
}

UnknownAddress::UnknownAddress(const sockaddr& address, socklen_t length)
    : Address(address.sa_family), length_(std::min<socklen_t>(
                                   length, sizeof(address_))) {
  std::memcpy(&address_, &address, length_);
}

UnknownAddress::UnknownAddress(int family) : Address(family) {
  address_.ss_family = static_cast<sa_family_t>(family);
  length_ = sizeof(sockaddr);
}

const sockaddr* UnknownAddress::getAddr() const noexcept {
  return reinterpret_cast<const sockaddr*>(&address_);
}

sockaddr* UnknownAddress::getAddr() noexcept {
  return reinterpret_cast<sockaddr*>(&address_);
}

socklen_t UnknownAddress::getAddrLen() const noexcept { return length_; }

std::ostream& UnknownAddress::insert(std::ostream& os) const {
  return os << "[UnknownAddress family=" << getFamily() << "]";
}

}  // namespace droplet
