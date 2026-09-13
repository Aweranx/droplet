#pragma once

#include <droplet/socket/socket.h>
#include <droplet/stream/stream.h>

#include <cstddef>
#include <memory>
#include <string>

namespace droplet {

/**
 * @brief 将 Socket 适配为 Stream 接口。
 *
 * SocketStream 不解析任何上层协议，只负责把 Socket 的字节收发映射为
 * Stream 的 read/write；HTTP、RPC 等协议可以在此基础上实现自己的会话类。
 */
class DROPLET_API SocketStream : public Stream {
 public:
  using Ptr = std::shared_ptr<SocketStream>;

  explicit SocketStream(Socket::Ptr socket, bool owner = true);
  ~SocketStream() override;

  SocketStream(const SocketStream&) = delete;
  SocketStream& operator=(const SocketStream&) = delete;

  int read(void* buffer, std::size_t length) override;
  int read(ByteArray::Ptr buffer, std::size_t length) override;
  int write(const void* buffer, std::size_t length) override;
  int write(ByteArray::Ptr buffer, std::size_t length) override;
  void close() override;

  [[nodiscard]] bool isConnected() const noexcept;
  [[nodiscard]] Socket::Ptr getSocket() const noexcept { return socket_; }
  [[nodiscard]] Address::Ptr getRemoteAddress() const;
  [[nodiscard]] Address::Ptr getLocalAddress() const;
  [[nodiscard]] std::string getRemoteAddressString() const;
  [[nodiscard]] std::string getLocalAddressString() const;

 private:
  Socket::Ptr socket_;
  bool owner_{true};
};

}  // namespace droplet
