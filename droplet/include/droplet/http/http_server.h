#pragma once

#include <droplet/export.h>
#include <droplet/http/servlet.h>
#include <droplet/tcpserver/tcp_server.h>

#include <atomic>
#include <memory>
#include <shared_mutex>

namespace droplet::http {

/** @brief 把 TcpServer、HttpSession 和 ServletDispatch 组合成 HTTP 服务端。 */
class DROPLET_API HttpServer final : public TcpServer {
public:
  using Ptr = std::shared_ptr<HttpServer>;

  explicit HttpServer(bool keep_alive = false,
                      IOManager *worker = IOManager::GetThis(),
                      IOManager *io_worker = IOManager::GetThis(),
                      IOManager *accept_worker = IOManager::GetThis());

  [[nodiscard]] bool isKeepAlive() const noexcept {
    return keep_alive_.load(std::memory_order_acquire);
  }
  void setKeepAlive(bool value) noexcept {
    keep_alive_.store(value, std::memory_order_release);
  }

  [[nodiscard]] ServletDispatch::Ptr getServletDispatch() const;
  void setServletDispatch(ServletDispatch::Ptr dispatch);
  void setName(std::string name) override;

protected:
  void handleClient(Socket::Ptr client) override;

private:
  std::atomic<bool> keep_alive_{false};
  mutable std::shared_mutex dispatch_mutex_;
  ServletDispatch::Ptr dispatch_;
};

} // namespace droplet::http
