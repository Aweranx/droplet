#include <droplet/http/http_session.h>

#include <droplet/logger/log.h>

#include <array>
#include <cerrno>
#include <utility>

namespace droplet::http {
namespace {

LoggerPtr HttpSessionLogger() {
  static auto logger = GetLogger("system");
  return logger;
}

} // namespace

HttpSession::HttpSession(Socket::Ptr socket, bool owner)
    : SocketStream(std::move(socket), owner) {
  input_buffer_.reserve(HttpRequestParser::GetHttpRequestBufferSize());
}

HttpRequest::Ptr HttpSession::recvRequest() {
  last_parse_error_ = HttpParseError::NONE;
  last_parse_error_message_.clear();
  HttpRequestParser parser;
  std::array<char, HttpRequestParser::kDefaultBufferSize> read_buffer{};

  while (true) {
    if (!input_buffer_.empty()) {
      const std::size_t buffered = input_buffer_.size();
      const std::size_t consumed =
          parser.execute(input_buffer_.data(), buffered);
      if (parser.hasError()) {
        last_parse_error_ = parser.getError();
        last_parse_error_message_ = parser.getErrorMessage();
        errno = EPROTO;
        DROPLET_LOG_DEBUG(HttpSessionLogger())
            << "HttpSession parse request failed: "
            << last_parse_error_message_;
        close();
        return nullptr;
      }
      if (parser.isFinished()) {
        input_buffer_.resize(buffered - consumed);
        return parser.getData();
      }
    }

    const int count = read(read_buffer.data(), read_buffer.size());
    if (count <= 0) {
      close();
      return nullptr;
    }
    input_buffer_.append(read_buffer.data(), static_cast<std::size_t>(count));
  }
}

int HttpSession::sendResponse(const HttpResponse::Ptr &response) {
  if (!response) {
    errno = EINVAL;
    return -1;
  }
  return sendResponse(*response);
}

int HttpSession::sendResponse(const HttpResponse &response) {
  const std::string data = response.toString();
  return writeFixSize(data.data(), data.size());
}

} // namespace droplet::http
