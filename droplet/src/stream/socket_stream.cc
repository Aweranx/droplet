#include "droplet/stream/socket_stream.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <vector>

namespace droplet {

SocketStream::SocketStream(Socket::Ptr socket, bool owner)
    : socket_(std::move(socket)), owner_(owner) {}

SocketStream::~SocketStream() {
  if (owner_) {
    close();
  }
}

bool SocketStream::isConnected() const noexcept {
  return socket_ && socket_->isConnected();
}

int SocketStream::read(void* buffer, std::size_t length) {
  if (!isConnected() || (!buffer && length != 0)) {
    return -1;
  }
  if (length > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
    errno = EMSGSIZE;
    return -1;
  }
  return static_cast<int>(socket_->recv(buffer, length));
}

int SocketStream::read(ByteArray::Ptr buffer, std::size_t length) {
  if (!isConnected() || (!buffer && length != 0)) {
    return -1;
  }
  if (length == 0) {
    return 0;
  }
  std::vector<iovec> buffers;
  const auto available = buffer->getWriteBuffers(buffers, length);
  if (available == 0 || buffers.empty()) {
    return 0;
  }
  const ssize_t result = socket_->recv(buffers.data(), buffers.size());
  if (result > 0) {
    buffer->setPosition(buffer->getPosition() +
                        static_cast<std::size_t>(result));
  }
  return static_cast<int>(result);
}

int SocketStream::write(const void* buffer, std::size_t length) {
  if (!isConnected() || (!buffer && length != 0)) {
    return -1;
  }
  if (length > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
    errno = EMSGSIZE;
    return -1;
  }
  return static_cast<int>(socket_->send(buffer, length));
}

int SocketStream::write(ByteArray::Ptr buffer, std::size_t length) {
  if (!isConnected() || (!buffer && length != 0)) {
    return -1;
  }
  if (length == 0) {
    return 0;
  }
  std::vector<iovec> buffers;
  const auto available = buffer->getReadBuffers(buffers, length);
  if (available == 0 || buffers.empty()) {
    return 0;
  }
  const ssize_t result = socket_->send(buffers.data(), buffers.size());
  if (result > 0) {
    buffer->setPosition(buffer->getPosition() +
                        static_cast<std::size_t>(result));
  }
  return static_cast<int>(result);
}

void SocketStream::close() {
  if (socket_) {
    (void)socket_->close();
  }
}

Address::Ptr SocketStream::getRemoteAddress() const {
  return socket_ ? socket_->getRemoteAddress() : nullptr;
}

Address::Ptr SocketStream::getLocalAddress() const {
  return socket_ ? socket_->getLocalAddress() : nullptr;
}

std::string SocketStream::getRemoteAddressString() const {
  const auto address = getRemoteAddress();
  return address ? address->toString() : std::string{};
}

std::string SocketStream::getLocalAddressString() const {
  const auto address = getLocalAddress();
  return address ? address->toString() : std::string{};
}

}  // namespace droplet
