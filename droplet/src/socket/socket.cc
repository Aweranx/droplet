#include "droplet/socket/socket.h"
#include <droplet/types.h>

#include <droplet/socket/fd_manager.h>
#include <droplet/hook/hook.h>
#include <droplet/iomanager/iomanager.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

namespace droplet {

Socket::Ptr Socket::CreateTCP(Address::Ptr address) {
  if (!address) {
    return nullptr;
  }
  return std::make_shared<Socket>(address->getFamily(), TCP, 0);
}

Socket::Ptr Socket::CreateUDP(Address::Ptr address) {
  if (!address) {
    return nullptr;
  }
  auto socket = std::make_shared<Socket>(address->getFamily(), UDP, 0);
  socket->newSock();
  socket->connected_ = socket->isValid();
  return socket;
}

Socket::Ptr Socket::CreateTCPSocket() {
  return std::make_shared<Socket>(IPv4, TCP, 0);
}

Socket::Ptr Socket::CreateUDPSocket() {
  return CreateUDP(std::make_shared<IPv4Address>());
}

Socket::Ptr Socket::CreateTCPSocket6() {
  return std::make_shared<Socket>(IPv6, TCP, 0);
}

Socket::Ptr Socket::CreateUDPSocket6() {
  return CreateUDP(std::make_shared<IPv6Address>());
}

Socket::Ptr Socket::CreateUnixTCPSocket() {
  return std::make_shared<Socket>(UNIX, TCP, 0);
}

Socket::Ptr Socket::CreateUnixUDPSocket() {
  return std::make_shared<Socket>(UNIX, UDP, 0);
}

// bind或accept时才延时创建socket
Socket::Socket(int family, int type, int protocol)
    : family_(family), type_(type), protocol_(protocol) {}

Socket::~Socket() { (void)close(); }

// 先去FdMgr的缓存查找，没有的时候才使用syscall获取
i64 Socket::getSendTimeout() const {
  if (auto context = FdMgr::GetInstance().get(socket_)) {
    const u64 timeout = context->getTimeout(SO_SNDTIMEO);
    return timeout == UINT64_MAX ? -1 : static_cast<i64>(timeout);
  }
  timeval value{};
  socklen_t length = sizeof(value);
  if (!getOption(SOL_SOCKET, SO_SNDTIMEO, &value, &length)) {
    return -1;
  }
  if (value.tv_sec == 0 && value.tv_usec == 0) {
    return -1;
  }
  return static_cast<i64>(value.tv_sec) * 1000 + value.tv_usec / 1000;
}

void Socket::setSendTimeout(i64 milliseconds) {
  if (milliseconds < 0) {
    milliseconds = 0;
  }
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  (void)setOption(SOL_SOCKET, SO_SNDTIMEO, value);
}

i64 Socket::getRecvTimeout() const {
  if (auto context = FdMgr::GetInstance().get(socket_)) {
    const u64 timeout = context->getTimeout(SO_RCVTIMEO);
    return timeout == UINT64_MAX ? -1 : static_cast<i64>(timeout);
  }
  timeval value{};
  socklen_t length = sizeof(value);
  if (!getOption(SOL_SOCKET, SO_RCVTIMEO, &value, &length)) {
    return -1;
  }
  if (value.tv_sec == 0 && value.tv_usec == 0) {
    return -1;
  }
  return static_cast<i64>(value.tv_sec) * 1000 + value.tv_usec / 1000;
}

void Socket::setRecvTimeout(i64 milliseconds) {
  if (milliseconds < 0) {
    milliseconds = 0;
  }
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  (void)setOption(SOL_SOCKET, SO_RCVTIMEO, value);
}

bool Socket::getOption(int level, int option, void* result,
                       socklen_t* length) const {
  return socket_ != -1 && result && length &&
         ::getsockopt(socket_, level, option, result, length) == 0;
}

bool Socket::setOption(int level, int option, const void* result,
                       socklen_t length) {
  return socket_ != -1 && result &&
         ::setsockopt(socket_, level, option, result, length) == 0;
}

Socket::Ptr Socket::accept() {
  if (socket_ == -1) {
    errno = EBADF;
    return nullptr;
  }
  const int accepted = ::accept(socket_, nullptr, nullptr);
  if (accepted == -1) {
    return nullptr;
  }

  auto result = std::make_shared<Socket>(family_, type_, protocol_);
  if (!result->init(accepted)) {
    (void)::close(accepted);
    return nullptr;
  }
  return result;
}

bool Socket::bind(const Address::Ptr& address) {
  if (!address || address->getFamily() != family_) {
    errno = EAFNOSUPPORT;
    return false;
  }
  if (socket_ == -1) {
    newSock();
  }
  if (socket_ == -1) {
    return false;
  }
  if (family_ == UNIX) {
    const auto* unix_address =
        dynamic_cast<const UnixAddress*>(address.get());
    // Filesystem Unix sockets leave a stale pathname after the previous
    // process exits.  Abstract-namespace sockets (sun_path[0] == '\0') do
    // not have a filesystem entry and must not be unlinked.
    if (unix_address) {
      const auto* raw = reinterpret_cast<const sockaddr_un*>(
          unix_address->getAddr());
      if (raw->sun_path[0] != '\0') {
        (void)::unlink(raw->sun_path);
      }
    }
  }
  if (socket_ == -1 || ::bind(socket_, address->getAddr(),
                               address->getAddrLen()) != 0) {
    return false;
  }
  local_address_ = getLocalAddress();
  return true;
}

bool Socket::connect(const Address::Ptr& address, u64 timeout_ms) {
  if (!address) {
    errno = EINVAL;
    return false;
  }
  remote_address_ = address;
  if (address->getFamily() != family_) {
    errno = EAFNOSUPPORT;
    return false;
  }
  if (socket_ == -1) {
    newSock();
  }
  if (socket_ == -1) {
    return false;
  }
  if (::connect_with_timeout(socket_, address->getAddr(), address->getAddrLen(),
                             timeout_ms) != 0) {
    (void)close();
    return false;
  }
  connected_ = true;
  remote_address_ = getRemoteAddress();
  local_address_ = getLocalAddress();
  return true;
}

bool Socket::reconnect(u64 timeout_ms) {
  if (!remote_address_) {
    errno = ENOTCONN;
    return false;
  }
  local_address_.reset();
  connected_ = false;
  return connect(remote_address_, timeout_ms);
}

bool Socket::listen(int backlog) {
  if (socket_ == -1) {
    errno = EBADF;
    return false;
  }
  return ::listen(socket_, backlog) == 0;
}

bool Socket::close() {
  if (socket_ == -1) {
    connected_ = false;
    return true;
  }
  const int result = ::close(socket_);
  socket_ = -1;
  connected_ = false;
  return result == 0;
}

ssize_t Socket::send(const void* buffer, size_t length, int flags) {
  return socket_ == -1 ? -1 : ::send(socket_, buffer, length, flags);
}

ssize_t Socket::send(const iovec* buffers, size_t length, int flags) {
  if (socket_ == -1 || !buffers) {
    errno = EBADF;
    return -1;
  }
  msghdr message{};
  message.msg_iov = const_cast<iovec*>(buffers);
  message.msg_iovlen = length;
  return ::sendmsg(socket_, &message, flags);
}

ssize_t Socket::sendTo(const void* buffer, size_t length,
                       const Address::Ptr& to, int flags) {
  if (socket_ == -1 || !to) {
    errno = EBADF;
    return -1;
  }
  return ::sendto(socket_, buffer, length, flags, to->getAddr(),
                  to->getAddrLen());
}

ssize_t Socket::sendTo(const iovec* buffers, size_t length,
                       const Address::Ptr& to, int flags) {
  if (socket_ == -1 || !buffers || !to) {
    errno = EBADF;
    return -1;
  }
  msghdr message{};
  message.msg_iov = const_cast<iovec*>(buffers);
  message.msg_iovlen = length;
  message.msg_name = const_cast<sockaddr*>(to->getAddr());
  message.msg_namelen = to->getAddrLen();
  return ::sendmsg(socket_, &message, flags);
}

ssize_t Socket::recv(void* buffer, size_t length, int flags) {
  return socket_ == -1 ? -1 : ::recv(socket_, buffer, length, flags);
}

ssize_t Socket::recv(iovec* buffers, size_t length, int flags) {
  if (socket_ == -1 || !buffers) {
    errno = EBADF;
    return -1;
  }
  msghdr message{};
  message.msg_iov = buffers;
  message.msg_iovlen = length;
  return ::recvmsg(socket_, &message, flags);
}

ssize_t Socket::recvFrom(void* buffer, size_t length, Address::Ptr from,
                         int flags) {
  if (socket_ == -1 || !from) {
    errno = EBADF;
    return -1;
  }
  socklen_t address_length = from->getAddrLen();
  return ::recvfrom(socket_, buffer, length, flags, from->getAddr(),
                    &address_length);
}

ssize_t Socket::recvFrom(iovec* buffers, size_t length, Address::Ptr from,
                         int flags) {
  if (socket_ == -1 || !buffers || !from) {
    errno = EBADF;
    return -1;
  }
  msghdr message{};
  message.msg_iov = buffers;
  message.msg_iovlen = length;
  message.msg_name = from->getAddr();
  message.msg_namelen = from->getAddrLen();
  return ::recvmsg(socket_, &message, flags);
}

Address::Ptr Socket::getRemoteAddress() {
  if (remote_address_) {
    return remote_address_;
  }
  if (socket_ == -1) {
    return nullptr;
  }
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getpeername(socket_, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    return std::make_shared<UnknownAddress>(family_);
  }
  remote_address_ = Address::Create(reinterpret_cast<sockaddr*>(&address),
                                    length);
  return remote_address_;
}

Address::Ptr Socket::getLocalAddress() {
  if (local_address_) {
    return local_address_;
  }
  if (socket_ == -1) {
    return nullptr;
  }
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    return std::make_shared<UnknownAddress>(family_);
  }
  local_address_ = Address::Create(reinterpret_cast<sockaddr*>(&address),
                                   length);
  return local_address_;
}

int Socket::getError() const {
  int error = 0;
  socklen_t length = sizeof(error);
  if (!getOption(SOL_SOCKET, SO_ERROR, &error, &length)) {
    return errno;
  }
  return error;
}

std::ostream& Socket::dump(std::ostream& os) const {
  os << "[Socket fd=" << socket_ << " connected=" << connected_
     << " family=" << family_ << " type=" << type_
     << " protocol=" << protocol_;
  if (local_address_) {
    os << " local=" << *local_address_;
  }
  if (remote_address_) {
    os << " remote=" << *remote_address_;
  }
  return os << ']';
}

std::string Socket::toString() const {
  std::ostringstream stream;
  dump(stream);
  return stream.str();
}

bool Socket::cancelRead() {
  auto* iom = IOManager::GetThis();
  return iom && iom->cancelEvent(socket_, IOManager::READ);
}

bool Socket::cancelWrite() {
  auto* iom = IOManager::GetThis();
  return iom && iom->cancelEvent(socket_, IOManager::WRITE);
}

bool Socket::cancelAccept() { return cancelRead(); }

bool Socket::cancelAll() {
  auto* iom = IOManager::GetThis();
  return iom && iom->cancelAll(socket_);
}

void Socket::initSock() {
  int value = 1;
  (void)setOption(SOL_SOCKET, SO_REUSEADDR, value);
  if (type_ == TCP) {
    (void)setOption(IPPROTO_TCP, TCP_NODELAY, value);
  }
}

void Socket::newSock() {
  if (socket_ != -1) {
    return;
  }
  socket_ = ::socket(family_, type_, protocol_);
  if (socket_ != -1) {
    initSock();
  }
}

bool Socket::init(int socket) {
  if (socket < 0) {
    return false;
  }
  socket_ = socket;
  connected_ = true;
  if (isHookEnabled()) {
    (void)FdMgr::GetInstance().get(socket_, true);
  }
  initSock();
  local_address_ = getLocalAddress();
  remote_address_ = getRemoteAddress();
  return true;
}

std::ostream& operator<<(std::ostream& os, const Socket& socket) {
  return socket.dump(os);
}

}  // namespace droplet
