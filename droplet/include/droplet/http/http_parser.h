#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/http/http.h>
#include <droplet/http/http_request.h>
#include <droplet/http/http_response.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace droplet::http {

enum class HttpParseError : u8 {
  NONE = 0,
  INVALID_METHOD,
  INVALID_REQUEST_TARGET,
  INVALID_VERSION,
  INVALID_STATUS,
  INVALID_HEADER,
  INVALID_CONTENT_LENGTH,
  INVALID_CHUNK_SIZE,
  UNSUPPORTED_TRANSFER_ENCODING,
  HEADER_TOO_LARGE,
  BODY_TOO_LARGE,
};

/**
 * @brief HTTP/1.0、HTTP/1.1 请求解析器。
 *
 * execute(char*, size_t) 与 Sylar 一样会把未消费数据移动到缓冲区开头，便于
 * 后续 HttpSession 解析流水线请求；传入不完整请求时返回 0，调用方应继续收包。
 */
class DROPLET_API HttpRequestParser {
public:
  using Ptr = std::shared_ptr<HttpRequestParser>;

  static constexpr std::size_t kDefaultBufferSize = 4 * 1024;
  static constexpr std::size_t kMaxHeaderSize = 64 * 1024;
  static constexpr std::size_t kMaxBodySize = 64 * 1024 * 1024;

  HttpRequestParser();

  std::size_t execute(char *data, std::size_t length);
  std::size_t execute(std::string_view data);

  [[nodiscard]] bool isFinished() const noexcept { return finished_; }
  [[nodiscard]] bool hasError() const noexcept {
    return error_ != HttpParseError::NONE;
  }
  [[nodiscard]] HttpParseError getError() const noexcept { return error_; }
  [[nodiscard]] const std::string &getErrorMessage() const noexcept {
    return error_message_;
  }
  [[nodiscard]] const HttpRequest::Ptr &getData() const noexcept {
    return data_;
  }
  [[nodiscard]] u64 getContentLength() const noexcept {
    return content_length_;
  }

  void reset();

  [[nodiscard]] static constexpr std::size_t
  GetHttpRequestBufferSize() noexcept {
    return kDefaultBufferSize;
  }
  [[nodiscard]] static constexpr std::size_t
  GetHttpRequestMaxBodySize() noexcept {
    return kMaxBodySize;
  }

private:
  std::size_t parse(std::string_view data);
  void setError(HttpParseError error, std::string message);

  HttpRequest::Ptr data_;
  HttpParseError error_{HttpParseError::NONE};
  std::string error_message_;
  u64 content_length_{0};
  bool finished_{false};
};

/**
 * @brief HTTP/1.0、HTTP/1.1 响应解析器。
 *
 * 支持 Content-Length、chunked 和由连接关闭定界的响应体。对于后一种响应，
 * 调用方在读到 EOF 后需以 eof=true 再执行一次解析。
 */
class DROPLET_API HttpResponseParser {
public:
  using Ptr = std::shared_ptr<HttpResponseParser>;

  static constexpr std::size_t kDefaultBufferSize = 4 * 1024;
  static constexpr std::size_t kMaxHeaderSize = 64 * 1024;
  static constexpr std::size_t kMaxBodySize = 64 * 1024 * 1024;

  explicit HttpResponseParser(HttpMethod request_method = HttpMethod::GET);

  std::size_t execute(char *data, std::size_t length, bool eof = false);
  std::size_t execute(std::string_view data, bool eof = false);

  [[nodiscard]] bool isFinished() const noexcept { return finished_; }
  [[nodiscard]] bool hasError() const noexcept {
    return error_ != HttpParseError::NONE;
  }
  [[nodiscard]] HttpParseError getError() const noexcept { return error_; }
  [[nodiscard]] const std::string &getErrorMessage() const noexcept {
    return error_message_;
  }
  [[nodiscard]] const HttpResponse::Ptr &getData() const noexcept {
    return data_;
  }
  [[nodiscard]] u64 getContentLength() const noexcept {
    return content_length_;
  }
  [[nodiscard]] bool isChunked() const noexcept { return chunked_; }
  [[nodiscard]] bool isCloseDelimited() const noexcept {
    return close_delimited_;
  }

  void reset(HttpMethod request_method = HttpMethod::GET);

  [[nodiscard]] static constexpr std::size_t
  GetHttpResponseBufferSize() noexcept {
    return kDefaultBufferSize;
  }
  [[nodiscard]] static constexpr std::size_t
  GetHttpResponseMaxBodySize() noexcept {
    return kMaxBodySize;
  }

private:
  std::size_t parse(std::string_view data, bool eof);
  void setError(HttpParseError error, std::string message);

  HttpResponse::Ptr data_;
  HttpMethod request_method_{HttpMethod::GET};
  HttpParseError error_{HttpParseError::NONE};
  std::string error_message_;
  u64 content_length_{0};
  bool chunked_{false};
  bool close_delimited_{false};
  bool finished_{false};
};

[[nodiscard]] DROPLET_API const char *
HttpParseErrorToString(HttpParseError error) noexcept;

} // namespace droplet::http
