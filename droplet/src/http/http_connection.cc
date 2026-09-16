#include <droplet/http/http_connection.h>
#include <droplet/types.h>

#include <droplet/http/http_parser.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include <sys/socket.h>

namespace droplet::http {
namespace {

[[nodiscard]] u64 NowMilliseconds() noexcept {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

[[nodiscard]] bool CaseInsensitiveEqual(std::string_view lhs,
                                        std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const unsigned char left = static_cast<unsigned char>(lhs[index]);
    const unsigned char right = static_cast<unsigned char>(rhs[index]);
    const unsigned char folded_left =
        left >= 'A' && left <= 'Z' ? left + ('a' - 'A') : left;
    const unsigned char folded_right =
        right >= 'A' && right <= 'Z' ? right + ('a' - 'A') : right;
    if (folded_left != folded_right) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool HeaderContainsToken(std::string_view header,
                                       std::string_view token) noexcept {
  std::size_t begin = 0;
  while (begin <= header.size()) {
    const std::size_t end = header.find(',', begin);
    std::string_view item = header.substr(begin, end == std::string_view::npos
                                                     ? header.size() - begin
                                                     : end - begin);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.remove_prefix(1);
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.remove_suffix(1);
    }
    if (CaseInsensitiveEqual(item, token)) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return false;
}

[[nodiscard]] bool IsTimeoutError(int error) noexcept {
  return error == ETIMEDOUT || error == EAGAIN || error == EWOULDBLOCK;
}

void SetSocketTimeouts(const Socket::Ptr &socket, u64 timeout_ms) {
  if (!socket || timeout_ms == UINT64_MAX) {
    return;
  }
  const u64 bounded = std::min<u64>(
      timeout_ms, static_cast<u64>(std::numeric_limits<i64>::max()));
  socket->setRecvTimeout(static_cast<i64>(bounded));
  socket->setSendTimeout(static_cast<i64>(bounded));
}

[[nodiscard]] std::string HostHeader(const Uri &uri) {
  std::string result;
  if (uri.getHost().find(':') != std::string::npos) {
    result = '[' + uri.getHost() + ']';
  } else {
    result = uri.getHost();
  }
  if (!uri.isDefaultPort() && uri.getPort() >= 0) {
    result += ':' + std::to_string(uri.getPort());
  }
  return result;
}

[[nodiscard]] std::string HostHeader(std::string_view host, u16 port,
                                     bool https) {
  std::string result;
  if (host.find(':') != std::string_view::npos) {
    result = '[' + std::string(host) + ']';
  } else {
    result = host;
  }
  if ((https && port != 443) || (!https && port != 80)) {
    result += ':' + std::to_string(port);
  }
  return result;
}

[[nodiscard]] HttpRequest::Ptr BuildRequest(HttpMethod method, std::string path,
                                            std::string query, bool close,
                                            const HttpMap &headers,
                                            const std::string &body,
                                            std::string default_host) {
  auto request = std::make_shared<HttpRequest>(0x11, close);
  request->setMethod(method);
  request->setPath(path.empty() ? "/" : std::move(path));
  request->setQuery(std::move(query));
  request->setBody(body);

  bool has_host = false;
  for (const auto &[name, value] : headers) {
    if (CaseInsensitiveEqual(name, "connection")) {
      if (HeaderContainsToken(value, "close")) {
        request->setClose(true);
      } else if (HeaderContainsToken(value, "keep-alive")) {
        request->setClose(false);
      }
      continue;
    }
    if (CaseInsensitiveEqual(name, "host") && !value.empty()) {
      has_host = true;
    }
    request->setHeader(name, value);
  }
  if (!has_host) {
    request->setHeader("Host", std::move(default_host));
  }
  return request;
}

[[nodiscard]] HttpResult::Ptr MakeResult(HttpResult::Error error,
                                         std::string message,
                                         HttpResponse::Ptr response = nullptr) {
  return std::make_shared<HttpResult>(error, std::move(response),
                                      std::move(message));
}

[[nodiscard]] bool IsInterimResponse(const HttpResponse &response) noexcept {
  const auto status = static_cast<u16>(response.getStatus());
  return status >= 100 && status < 200 && status != 101;
}

} // namespace

HttpResult::HttpResult(Error code, HttpResponse::Ptr value, std::string message)
    : result(static_cast<int>(code)), response(std::move(value)),
      error(std::move(message)) {}

std::string HttpResult::toString() const {
  std::ostringstream stream;
  stream << "[HttpResult result=" << result << " error=" << error
         << " response=" << (response ? response->toString() : "nullptr")
         << ']';
  return stream.str();
}

HttpConnection::HttpConnection(Socket::Ptr socket, bool owner)
    : SocketStream(std::move(socket), owner),
      created_at_ms_(NowMilliseconds()) {
  input_buffer_.reserve(HttpResponseParser::GetHttpResponseBufferSize());
}

HttpResult::Ptr HttpConnection::DoGet(const std::string &url, u64 timeout_ms,
                                      const HttpMap &headers,
                                      const std::string &body) {
  return DoRequest(HttpMethod::GET, url, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnection::DoGet(const Uri::Ptr &uri, u64 timeout_ms,
                                      const HttpMap &headers,
                                      const std::string &body) {
  return DoRequest(HttpMethod::GET, uri, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnection::DoPost(const std::string &url, u64 timeout_ms,
                                       const HttpMap &headers,
                                       const std::string &body) {
  return DoRequest(HttpMethod::POST, url, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnection::DoPost(const Uri::Ptr &uri, u64 timeout_ms,
                                       const HttpMap &headers,
                                       const std::string &body) {
  return DoRequest(HttpMethod::POST, uri, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnection::DoRequest(HttpMethod method,
                                          const std::string &url,
                                          u64 timeout_ms,
                                          const HttpMap &headers,
                                          const std::string &body) {
  auto uri = Uri::Create(url);
  if (!uri) {
    return MakeResult(HttpResult::Error::INVALID_URL, "invalid URL: " + url);
  }
  return DoRequest(method, uri, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnection::DoRequest(HttpMethod method,
                                          const Uri::Ptr &uri, u64 timeout_ms,
                                          const HttpMap &headers,
                                          const std::string &body) {
  if (!uri) {
    return MakeResult(HttpResult::Error::INVALID_URL, "URI is null");
  }
  if (uri->getScheme() != "http") {
    return MakeResult(
        HttpResult::Error::UNSUPPORTED_SCHEME,
        "only plain http:// is supported; TLS is not implemented");
  }
  if (uri->getHost().empty()) {
    return MakeResult(HttpResult::Error::INVALID_HOST,
                      "HTTP URI has an empty host");
  }
  auto request = BuildRequest(method, uri->getPath(), uri->getQuery(), true,
                              headers, body, HostHeader(*uri));
  return DoRequest(request, uri, timeout_ms);
}

HttpResult::Ptr HttpConnection::DoRequest(const HttpRequest::Ptr &request,
                                          const Uri::Ptr &uri, u64 timeout_ms) {
  if (!request || !uri) {
    return MakeResult(HttpResult::Error::INVALID_URL,
                      "request and URI must not be null");
  }
  if (uri->getScheme() != "http") {
    return MakeResult(
        HttpResult::Error::UNSUPPORTED_SCHEME,
        "only plain http:// is supported; TLS is not implemented");
  }
  auto address = uri->createAddress();
  if (!address) {
    return MakeResult(HttpResult::Error::INVALID_HOST,
                      "cannot resolve host: " + uri->getHost());
  }
  auto socket = Socket::CreateTCP(address);
  if (!socket) {
    return MakeResult(HttpResult::Error::CREATE_SOCKET_ERROR,
                      "cannot create socket for " + address->toString());
  }
  if (!socket->connect(address, timeout_ms)) {
    const int error = errno;
    return MakeResult(IsTimeoutError(error) ? HttpResult::Error::TIMEOUT
                                            : HttpResult::Error::CONNECT_FAIL,
                      "connect to " + address->toString() +
                          " failed: " + std::strerror(error));
  }
  SetSocketTimeouts(socket, timeout_ms);

  auto connection = std::make_shared<HttpConnection>(socket);
  const int sent = connection->sendRequest(request);
  if (sent == 0) {
    return MakeResult(HttpResult::Error::SEND_CLOSE_BY_PEER,
                      "peer closed while sending HTTP request");
  }
  if (sent < 0) {
    const int error = errno;
    return MakeResult(
        IsTimeoutError(error) ? HttpResult::Error::TIMEOUT
                              : HttpResult::Error::SEND_SOCKET_ERROR,
        "send HTTP request failed: " + std::string(std::strerror(error)));
  }
  auto response = connection->recvResponse();
  if (!response) {
    return MakeResult(connection->getLastError(),
                      connection->getLastErrorMessage());
  }
  return MakeResult(HttpResult::Error::OK, "ok", std::move(response));
}

int HttpConnection::sendRequest(const HttpRequest::Ptr &request) {
  if (!request) {
    errno = EINVAL;
    setLastError(HttpResult::Error::SEND_SOCKET_ERROR, "request is null");
    return -1;
  }
  const std::string data = request->toString();
  pending_method_ = request->getMethod();
  reusable_ = false;
  const int result = writeFixSize(data.data(), data.size());
  if (result <= 0) {
    const int error = errno;
    setLastError(result == 0 ? HttpResult::Error::SEND_CLOSE_BY_PEER
                             : (IsTimeoutError(error)
                                    ? HttpResult::Error::TIMEOUT
                                    : HttpResult::Error::SEND_SOCKET_ERROR),
                 result == 0 ? "peer closed while sending HTTP request"
                             : "send HTTP request failed: " +
                                   std::string(std::strerror(error)));
    close();
  }
  return result;
}

HttpResponse::Ptr HttpConnection::recvResponse() {
  last_error_ = HttpResult::Error::OK;
  last_error_message_.clear();
  HttpResponseParser parser(pending_method_);
  std::array<char, HttpResponseParser::kDefaultBufferSize> read_buffer{};

  while (true) {
    if (!input_buffer_.empty()) {
      const std::size_t buffered = input_buffer_.size();
      const std::size_t consumed =
          parser.execute(input_buffer_.data(), buffered, false);
      if (parser.hasError()) {
        setLastError(HttpResult::Error::PARSE_ERROR, parser.getErrorMessage());
        close();
        return nullptr;
      }
      if (parser.isFinished()) {
        input_buffer_.resize(buffered - consumed);
        auto response = parser.getData();
        if (response && IsInterimResponse(*response)) {
          parser.reset(pending_method_);
          continue;
        }
        ++request_count_;
        reusable_ = response && !response->isClose() &&
                    response->getStatus() != HttpStatus::SWITCHING_PROTOCOLS;
        return response;
      }
    }

    const int count = read(read_buffer.data(), read_buffer.size());
    if (count > 0) {
      input_buffer_.append(read_buffer.data(), static_cast<std::size_t>(count));
      continue;
    }

    if (count == 0 && !input_buffer_.empty()) {
      const std::size_t buffered = input_buffer_.size();
      const std::size_t consumed =
          parser.execute(input_buffer_.data(), buffered, true);
      if (parser.isFinished()) {
        input_buffer_.resize(buffered - consumed);
        auto response = parser.getData();
        ++request_count_;
        reusable_ = false;
        close();
        return response;
      }
      if (parser.hasError()) {
        setLastError(HttpResult::Error::PARSE_ERROR, parser.getErrorMessage());
        close();
        return nullptr;
      }
    }

    const int error = errno;
    if (count == 0) {
      setLastError(HttpResult::Error::RECEIVE_CLOSE_BY_PEER,
                   "peer closed before a complete HTTP response was received");
    } else {
      setLastError(
          IsTimeoutError(error) ? HttpResult::Error::TIMEOUT
                                : HttpResult::Error::RECEIVE_SOCKET_ERROR,
          "receive HTTP response failed: " + std::string(std::strerror(error)));
    }
    close();
    return nullptr;
  }
}

void HttpConnection::setLastError(HttpResult::Error error,
                                  std::string message) {
  last_error_ = error;
  last_error_message_ = std::move(message);
}

HttpConnectionPool::Ptr HttpConnectionPool::Create(const std::string &origin,
                                                   std::string vhost,
                                                   u32 max_size,
                                                   u64 max_alive_time_ms,
                                                   u32 max_requests) {
  auto uri = Uri::Create(origin);
  if (!uri || uri->getHost().empty() || uri->getPort() < 0 ||
      uri->getPort() > std::numeric_limits<u16>::max() ||
      (uri->getScheme() != "http" && uri->getScheme() != "https") ||
      max_size == 0) {
    return nullptr;
  }
  return Ptr(new HttpConnectionPool(
      uri->getHost(), std::move(vhost), static_cast<u16>(uri->getPort()),
      uri->getScheme() == "https", max_size, max_alive_time_ms, max_requests));
}

HttpConnectionPool::HttpConnectionPool(std::string host, std::string vhost,
                                       u16 port, bool is_https, u32 max_size,
                                       u64 max_alive_time_ms, u32 max_requests)
    : host_(std::move(host)), vhost_(std::move(vhost)), port_(port),
      is_https_(is_https), max_size_(max_size),
      max_alive_time_ms_(max_alive_time_ms), max_requests_(max_requests) {}

HttpConnectionPool::~HttpConnectionPool() {
  std::deque<HttpConnection *> connections;
  {
    std::lock_guard lock(mutex_);
    connections.swap(idle_connections_);
  }
  for (HttpConnection *connection : connections) {
    delete connection;
  }
  total_.fetch_sub(static_cast<u32>(connections.size()),
                   std::memory_order_acq_rel);
}

HttpConnection::Ptr HttpConnectionPool::getConnection(u64 timeout_ms) {
  if (is_https_ || max_size_ == 0) {
    return nullptr;
  }

  std::vector<HttpConnection *> invalid;
  HttpConnection *connection = nullptr;
  bool reserved_slot = false;
  const u64 now = NowMilliseconds();
  {
    std::lock_guard lock(mutex_);
    while (!idle_connections_.empty()) {
      HttpConnection *candidate = idle_connections_.front();
      idle_connections_.pop_front();
      if (!candidate->isConnected() || !candidate->reusable_ ||
          isExpired(*candidate, now)) {
        invalid.push_back(candidate);
      } else {
        connection = candidate;
        break;
      }
    }
    if (!connection) {
      const u32 after_invalid = total_.load(std::memory_order_relaxed) -
                                static_cast<u32>(invalid.size());
      if (after_invalid < max_size_) {
        total_.fetch_add(1, std::memory_order_relaxed);
        reserved_slot = true;
      }
    }
  }

  for (HttpConnection *candidate : invalid) {
    delete candidate;
  }
  if (!invalid.empty()) {
    total_.fetch_sub(static_cast<u32>(invalid.size()),
                     std::memory_order_acq_rel);
  }

  if (!connection && reserved_slot) {
    auto address = Address::LookupAnyIPAddress(host_, AF_UNSPEC, SOCK_STREAM);
    if (address) {
      address->setPort(port_);
      auto socket = Socket::CreateTCP(address);
      if (socket && socket->connect(address, timeout_ms)) {
        SetSocketTimeouts(socket, timeout_ms);
        connection = new HttpConnection(std::move(socket));
      }
    }
    if (!connection) {
      total_.fetch_sub(1, std::memory_order_acq_rel);
      return nullptr;
    }
  }
  if (!connection) {
    return nullptr;
  }

  const std::weak_ptr<HttpConnectionPool> weak_pool = weak_from_this();
  return HttpConnection::Ptr(connection,
                             [weak_pool](HttpConnection *released) noexcept {
                               if (auto pool = weak_pool.lock()) {
                                 pool->releaseConnection(released);
                               } else {
                                 delete released;
                               }
                             });
}

HttpResult::Ptr HttpConnectionPool::doGet(std::string_view target,
                                          u64 timeout_ms,
                                          const HttpMap &headers,
                                          const std::string &body) {
  return doRequest(HttpMethod::GET, target, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnectionPool::doGet(const Uri::Ptr &uri, u64 timeout_ms,
                                          const HttpMap &headers,
                                          const std::string &body) {
  return doRequest(HttpMethod::GET, uri, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnectionPool::doPost(std::string_view target,
                                           u64 timeout_ms,
                                           const HttpMap &headers,
                                           const std::string &body) {
  return doRequest(HttpMethod::POST, target, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnectionPool::doPost(const Uri::Ptr &uri, u64 timeout_ms,
                                           const HttpMap &headers,
                                           const std::string &body) {
  return doRequest(HttpMethod::POST, uri, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              std::string_view target,
                                              u64 timeout_ms,
                                              const HttpMap &headers,
                                              const std::string &body) {
  if (is_https_) {
    return MakeResult(HttpResult::Error::UNSUPPORTED_SCHEME,
                      "HTTPS connection pools require TLS support");
  }
  const std::size_t fragment = target.find('#');
  if (fragment != std::string_view::npos) {
    target = target.substr(0, fragment);
  }
  const std::size_t query = target.find('?');
  std::string path(target.substr(0, query));
  std::string query_text = query == std::string_view::npos
                               ? std::string{}
                               : std::string(target.substr(query + 1));
  if (path.empty()) {
    path = "/";
  }
  if (!path.starts_with('/')) {
    return MakeResult(HttpResult::Error::INVALID_URL,
                      "connection-pool request target must start with '/'");
  }
  const std::string default_host =
      vhost_.empty() ? HostHeader(host_, port_, is_https_) : vhost_;
  auto request = BuildRequest(method, std::move(path), std::move(query_text),
                              false, headers, body, default_host);
  return doRequest(request, timeout_ms);
}

HttpResult::Ptr HttpConnectionPool::doRequest(HttpMethod method,
                                              const Uri::Ptr &uri,
                                              u64 timeout_ms,
                                              const HttpMap &headers,
                                              const std::string &body) {
  if (!uri) {
    return MakeResult(HttpResult::Error::INVALID_URL, "URI is null");
  }
  if (!CaseInsensitiveEqual(uri->getHost(), host_) || uri->getPort() != port_ ||
      uri->getScheme() != (is_https_ ? "https" : "http")) {
    return MakeResult(HttpResult::Error::INVALID_HOST,
                      "URI origin does not match the connection pool");
  }
  std::string target = uri->getPath();
  if (!uri->getQuery().empty()) {
    target += '?' + uri->getQuery();
  }
  return doRequest(method, target, timeout_ms, headers, body);
}

HttpResult::Ptr HttpConnectionPool::doRequest(const HttpRequest::Ptr &request,
                                              u64 timeout_ms) {
  if (!request) {
    return MakeResult(HttpResult::Error::INVALID_URL, "request is null");
  }
  if (is_https_) {
    return MakeResult(HttpResult::Error::UNSUPPORTED_SCHEME,
                      "HTTPS connection pools require TLS support");
  }
  auto connection = getConnection(timeout_ms);
  if (!connection) {
    return MakeResult(HttpResult::Error::POOL_GET_CONNECTION,
                      "cannot obtain an HTTP connection from the pool");
  }
  if (!connection->isConnected()) {
    return MakeResult(HttpResult::Error::POOL_INVALID_CONNECTION,
                      "connection pool returned a disconnected connection");
  }
  SetSocketTimeouts(connection->getSocket(), timeout_ms);
  const int sent = connection->sendRequest(request);
  if (sent == 0) {
    return MakeResult(HttpResult::Error::SEND_CLOSE_BY_PEER,
                      "peer closed while sending pooled HTTP request");
  }
  if (sent < 0) {
    return MakeResult(connection->getLastError(),
                      connection->getLastErrorMessage());
  }
  auto response = connection->recvResponse();
  if (!response) {
    return MakeResult(connection->getLastError(),
                      connection->getLastErrorMessage());
  }
  return MakeResult(HttpResult::Error::OK, "ok", std::move(response));
}

std::size_t HttpConnectionPool::getIdleConnectionCount() const {
  std::lock_guard lock(mutex_);
  return idle_connections_.size();
}

void HttpConnectionPool::releaseConnection(
    HttpConnection *connection) noexcept {
  if (!connection) {
    return;
  }
  const u64 now = NowMilliseconds();
  if (!connection->isConnected() || !connection->reusable_ ||
      isExpired(*connection, now)) {
    delete connection;
    total_.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }
  try {
    std::lock_guard lock(mutex_);
    idle_connections_.push_back(connection);
  } catch (...) {
    // shared_ptr deleter 不能传播异常；回收到池失败时直接销毁连接。
    delete connection;
    total_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

bool HttpConnectionPool::isExpired(const HttpConnection &connection,
                                   u64 now_ms) const noexcept {
  const bool lifetime_expired =
      max_alive_time_ms_ != 0 &&
      now_ms - connection.created_at_ms_ >= max_alive_time_ms_;
  const bool request_limit_reached =
      max_requests_ != 0 && connection.request_count_ >= max_requests_;
  return lifetime_expired || request_limit_reached;
}

} // namespace droplet::http
