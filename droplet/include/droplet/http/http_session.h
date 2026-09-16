#pragma once

#include <droplet/export.h>
#include <droplet/http/http_parser.h>
#include <droplet/http/http_request.h>
#include <droplet/http/http_response.h>
#include <droplet/stream/socket_stream.h>

#include <cstddef>
#include <memory>
#include <string>

namespace droplet::http {

/**
 * @brief 服务端视角的一条 HTTP 连接。
 *
 * HttpSession 把 SocketStream 中的字节解析成 HttpRequest，并保留一次读取中
 * 属于后续流水线请求的数据；每个 Session 应只由一个执行流调用。
 */
class DROPLET_API HttpSession : public SocketStream {
public:
  using Ptr = std::shared_ptr<HttpSession>;

  explicit HttpSession(Socket::Ptr socket, bool owner = true);

  /** @return 成功时返回请求；对端关闭、读取失败或协议错误时返回 nullptr。 */
  [[nodiscard]] HttpRequest::Ptr recvRequest();

  /** @return >0 表示发送字节数，=0 表示连接关闭，<0 表示发送失败。 */
  int sendResponse(const HttpResponse::Ptr &response);
  int sendResponse(const HttpResponse &response);

  [[nodiscard]] HttpParseError getLastParseError() const noexcept {
    return last_parse_error_;
  }
  [[nodiscard]] const std::string &getLastParseErrorMessage() const noexcept {
    return last_parse_error_message_;
  }
  [[nodiscard]] std::size_t getPendingInputSize() const noexcept {
    return input_buffer_.size();
  }

private:
  std::string input_buffer_;
  HttpParseError last_parse_error_{HttpParseError::NONE};
  std::string last_parse_error_message_;
};

} // namespace droplet::http
