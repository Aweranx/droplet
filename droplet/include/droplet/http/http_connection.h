#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/http/http.h>
#include <droplet/http/http_request.h>
#include <droplet/http/http_response.h>
#include <droplet/stream/socket_stream.h>
#include <droplet/uri/uri.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace droplet::http {

/** @brief 一次 HTTP 客户端操作的结果。 */
struct DROPLET_API HttpResult {
  using Ptr = std::shared_ptr<HttpResult>;

  enum class Error : int {
    OK = 0,
    INVALID_URL,
    INVALID_HOST,
    CONNECT_FAIL,
    SEND_CLOSE_BY_PEER,
    SEND_SOCKET_ERROR,
    TIMEOUT,
    CREATE_SOCKET_ERROR,
    POOL_GET_CONNECTION,
    POOL_INVALID_CONNECTION,
    UNSUPPORTED_SCHEME,
    RECEIVE_CLOSE_BY_PEER,
    RECEIVE_SOCKET_ERROR,
    PARSE_ERROR,
  };

  HttpResult(Error code, HttpResponse::Ptr response, std::string error);

  [[nodiscard]] bool success() const noexcept {
    return result == static_cast<int>(Error::OK);
  }
  [[nodiscard]] std::string toString() const;

  // 保持与 Sylar 客户端接口一致，result 使用可直接输出的整数错误码。
  int result{};
  HttpResponse::Ptr response;
  std::string error;
};

class HttpConnectionPool;

/** @brief 客户端视角的一条 HTTP/1.x 连接。 */
class DROPLET_API HttpConnection : public SocketStream {
  friend class HttpConnectionPool;

public:
  using Ptr = std::shared_ptr<HttpConnection>;

  static HttpResult::Ptr DoGet(const std::string &url, u64 timeout_ms,
                               const HttpMap &headers = {},
                               const std::string &body = {});
  static HttpResult::Ptr DoGet(const Uri::Ptr &uri, u64 timeout_ms,
                               const HttpMap &headers = {},
                               const std::string &body = {});
  static HttpResult::Ptr DoPost(const std::string &url, u64 timeout_ms,
                                const HttpMap &headers = {},
                                const std::string &body = {});
  static HttpResult::Ptr DoPost(const Uri::Ptr &uri, u64 timeout_ms,
                                const HttpMap &headers = {},
                                const std::string &body = {});
  static HttpResult::Ptr DoRequest(HttpMethod method, const std::string &url,
                                   u64 timeout_ms, const HttpMap &headers = {},
                                   const std::string &body = {});
  static HttpResult::Ptr DoRequest(HttpMethod method, const Uri::Ptr &uri,
                                   u64 timeout_ms, const HttpMap &headers = {},
                                   const std::string &body = {});
  static HttpResult::Ptr DoRequest(const HttpRequest::Ptr &request,
                                   const Uri::Ptr &uri, u64 timeout_ms);

  explicit HttpConnection(Socket::Ptr socket, bool owner = true);

  [[nodiscard]] HttpResponse::Ptr recvResponse();
  int sendRequest(const HttpRequest::Ptr &request);

  [[nodiscard]] HttpResult::Error getLastError() const noexcept {
    return last_error_;
  }
  [[nodiscard]] const std::string &getLastErrorMessage() const noexcept {
    return last_error_message_;
  }
  [[nodiscard]] u64 getRequestCount() const noexcept { return request_count_; }

private:
  void setLastError(HttpResult::Error error, std::string message);

  u64 created_at_ms_{};
  u64 request_count_{};
  HttpMethod pending_method_{HttpMethod::GET};
  bool reusable_{true};
  std::string input_buffer_;
  HttpResult::Error last_error_{HttpResult::Error::OK};
  std::string last_error_message_;
};

/** @brief 按 HTTP origin 复用持久连接的线程安全连接池。 */
class DROPLET_API HttpConnectionPool
    : public std::enable_shared_from_this<HttpConnectionPool> {
public:
  using Ptr = std::shared_ptr<HttpConnectionPool>;

  static Ptr Create(const std::string &origin, std::string vhost, u32 max_size,
                    u64 max_alive_time_ms, u32 max_requests);

  ~HttpConnectionPool();

  HttpConnectionPool(const HttpConnectionPool &) = delete;
  HttpConnectionPool &operator=(const HttpConnectionPool &) = delete;

  /** @brief 获取一个独占连接租约；容量已满或连接失败时返回 nullptr。 */
  [[nodiscard]] HttpConnection::Ptr getConnection(u64 timeout_ms = UINT64_MAX);

  HttpResult::Ptr doGet(std::string_view target, u64 timeout_ms,
                        const HttpMap &headers = {},
                        const std::string &body = {});
  HttpResult::Ptr doGet(const Uri::Ptr &uri, u64 timeout_ms,
                        const HttpMap &headers = {},
                        const std::string &body = {});
  HttpResult::Ptr doPost(std::string_view target, u64 timeout_ms,
                         const HttpMap &headers = {},
                         const std::string &body = {});
  HttpResult::Ptr doPost(const Uri::Ptr &uri, u64 timeout_ms,
                         const HttpMap &headers = {},
                         const std::string &body = {});
  HttpResult::Ptr doRequest(HttpMethod method, std::string_view target,
                            u64 timeout_ms, const HttpMap &headers = {},
                            const std::string &body = {});
  HttpResult::Ptr doRequest(HttpMethod method, const Uri::Ptr &uri,
                            u64 timeout_ms, const HttpMap &headers = {},
                            const std::string &body = {});
  HttpResult::Ptr doRequest(const HttpRequest::Ptr &request, u64 timeout_ms);

  [[nodiscard]] const std::string &getHost() const noexcept { return host_; }
  [[nodiscard]] u16 getPort() const noexcept { return port_; }
  [[nodiscard]] u32 getMaxSize() const noexcept { return max_size_; }
  [[nodiscard]] std::size_t getIdleConnectionCount() const;
  [[nodiscard]] u32 getTotalConnectionCount() const noexcept {
    return total_.load(std::memory_order_acquire);
  }

private:
  HttpConnectionPool(std::string host, std::string vhost, u16 port,
                     bool is_https, u32 max_size, u64 max_alive_time_ms,
                     u32 max_requests);

  void releaseConnection(HttpConnection *connection) noexcept;
  [[nodiscard]] bool isExpired(const HttpConnection &connection,
                               u64 now_ms) const noexcept;

  std::string host_;
  std::string vhost_;
  u16 port_{};
  bool is_https_{};
  u32 max_size_{};
  u64 max_alive_time_ms_{};
  u32 max_requests_{};

  mutable std::mutex mutex_;
  std::deque<HttpConnection *> idle_connections_;
  std::atomic<u32> total_{0};
};

} // namespace droplet::http
