#include <droplet/http/http_parser.h>
#include <droplet/types.h>

#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>

namespace droplet::http {
namespace {

[[nodiscard]] constexpr bool IsTokenCharacter(unsigned char value) noexcept {
  if (value <= 0x20 || value >= 0x7F) {
    return false;
  }
  switch (value) {
  case '(':
  case ')':
  case '<':
  case '>':
  case '@':
  case ',':
  case ';':
  case ':':
  case '\\':
  case '"':
  case '/':
  case '[':
  case ']':
  case '?':
  case '=':
  case '{':
  case '}':
    return false;
  default:
    return true;
  }
}

[[nodiscard]] bool IsValidHeaderName(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  for (const unsigned char character : name) {
    if (!IsTokenCharacter(character)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool IsValidHeaderValue(std::string_view value) noexcept {
  for (const unsigned char character : value) {
    if ((character < 0x20 && character != '\t') || character == 0x7F) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string_view
TrimOptionalWhitespace(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool CaseInsensitiveEqual(std::string_view lhs,
                                        std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    unsigned char left = static_cast<unsigned char>(lhs[index]);
    unsigned char right = static_cast<unsigned char>(rhs[index]);
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<unsigned char>(left + ('a' - 'A'));
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<unsigned char>(right + ('a' - 'A'));
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool ParseContentLength(std::string_view value,
                                      u64 &result) noexcept {
  value = TrimOptionalWhitespace(value);
  if (value.empty()) {
    return false;
  }
  const char *begin = value.data();
  const char *end = begin + value.size();
  const auto [position, error] = std::from_chars(begin, end, result);
  return error == std::errc{} && position == end;
}

[[nodiscard]] bool HeaderContainsFinalChunked(std::string_view value) noexcept {
  bool saw_token = false;
  bool saw_chunked = false;
  bool final_chunked = false;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t end = value.find(',', begin);
    std::string_view token = TrimOptionalWhitespace(
        value.substr(begin, end == std::string_view::npos ? value.size() - begin
                                                          : end - begin));
    if (token.empty()) {
      return false;
    }
    const std::size_t semicolon = token.find(';');
    const std::string_view coding =
        TrimOptionalWhitespace(token.substr(0, semicolon));
    if (coding.empty()) {
      return false;
    }
    if (saw_chunked) {
      // chunked 必须只出现一次且位于编码链的最后。
      return false;
    }
    saw_token = true;
    final_chunked = CaseInsensitiveEqual(coding, "chunked");
    saw_chunked = final_chunked;
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return saw_token && final_chunked;
}

[[nodiscard]] bool ResponseHasNoBody(HttpMethod request_method,
                                     u16 status) noexcept {
  return request_method == HttpMethod::HEAD ||
         (request_method == HttpMethod::CONNECT && status >= 200 &&
          status < 300) ||
         (status >= 100 && status < 200) || status == 204 || status == 304;
}

[[nodiscard]] bool ParseChunkSize(std::string_view line, u64 &result) noexcept {
  const std::size_t semicolon = line.find(';');
  line = TrimOptionalWhitespace(line.substr(0, semicolon));
  if (line.empty()) {
    return false;
  }
  const auto [position, error] =
      std::from_chars(line.data(), line.data() + line.size(), result, 16);
  return error == std::errc{} && position == line.data() + line.size();
}

} // namespace

HttpRequestParser::HttpRequestParser()
    : data_(std::make_shared<HttpRequest>()) {}

std::size_t HttpRequestParser::execute(char *data, std::size_t length) {
  if (length == 0 || data == nullptr || finished_ || hasError()) {
    return 0;
  }
  const std::size_t consumed = parse(std::string_view(data, length));
  if (consumed != 0 && consumed < length) {
    std::memmove(data, data + consumed, length - consumed);
  }
  return consumed;
}

std::size_t HttpRequestParser::execute(std::string_view data) {
  if (finished_ || hasError()) {
    return 0;
  }
  return parse(data);
}

void HttpRequestParser::reset() {
  data_ = std::make_shared<HttpRequest>();
  error_ = HttpParseError::NONE;
  error_message_.clear();
  content_length_ = 0;
  finished_ = false;
}

std::size_t HttpRequestParser::parse(std::string_view input) {
  const std::size_t header_end = input.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    if (input.size() > kMaxHeaderSize) {
      setError(HttpParseError::HEADER_TOO_LARGE,
               "HTTP request headers exceed the configured limit");
    }
    return 0;
  }

  const std::size_t header_size = header_end + 4;
  if (header_size > kMaxHeaderSize) {
    setError(HttpParseError::HEADER_TOO_LARGE,
             "HTTP request headers exceed the configured limit");
    return 0;
  }

  const std::size_t request_line_end = input.find("\r\n");
  if (request_line_end == std::string_view::npos ||
      request_line_end > header_end) {
    setError(HttpParseError::INVALID_VERSION,
             "HTTP request line is incomplete");
    return 0;
  }

  const std::string_view request_line = input.substr(0, request_line_end);
  const std::size_t method_end = request_line.find(' ');
  const std::size_t target_end = method_end == std::string_view::npos
                                     ? std::string_view::npos
                                     : request_line.find(' ', method_end + 1);
  if (method_end == std::string_view::npos ||
      target_end == std::string_view::npos || method_end == 0 ||
      target_end == method_end + 1 ||
      request_line.find(' ', target_end + 1) != std::string_view::npos) {
    setError(HttpParseError::INVALID_REQUEST_TARGET,
             "HTTP request line must contain method, target and version");
    return 0;
  }

  const std::string_view method_text = request_line.substr(0, method_end);
  const HttpMethod method = StringToHttpMethod(method_text);
  if (method == HttpMethod::INVALID_METHOD) {
    setError(HttpParseError::INVALID_METHOD,
             "unsupported HTTP method: " + std::string(method_text));
    return 0;
  }

  const std::string_view target =
      request_line.substr(method_end + 1, target_end - method_end - 1);
  for (const unsigned char character : target) {
    if (character <= 0x20 || character == 0x7F) {
      setError(HttpParseError::INVALID_REQUEST_TARGET,
               "HTTP request target contains an invalid character");
      return 0;
    }
  }

  const std::string_view version = request_line.substr(target_end + 1);
  u8 encoded_version = 0;
  if (version == "HTTP/1.1") {
    encoded_version = 0x11;
  } else if (version == "HTTP/1.0") {
    encoded_version = 0x10;
  } else {
    setError(HttpParseError::INVALID_VERSION,
             "only HTTP/1.0 and HTTP/1.1 requests are supported");
    return 0;
  }

  auto request =
      std::make_shared<HttpRequest>(encoded_version, encoded_version <= 0x10);
  request->setMethod(method);

  std::string_view target_without_fragment = target;
  const std::size_t fragment_position = target.find('#');
  if (fragment_position != std::string_view::npos) {
    request->setFragment(std::string(target.substr(fragment_position + 1)));
    target_without_fragment = target.substr(0, fragment_position);
  }
  const std::size_t query_position = target_without_fragment.find('?');
  if (query_position != std::string_view::npos) {
    request->setPath(
        std::string(target_without_fragment.substr(0, query_position)));
    request->setQuery(
        std::string(target_without_fragment.substr(query_position + 1)));
  } else {
    request->setPath(std::string(target_without_fragment));
  }
  if (request->getPath().empty()) {
    setError(HttpParseError::INVALID_REQUEST_TARGET,
             "HTTP request target has an empty path");
    return 0;
  }

  bool content_length_seen = false;
  u64 parsed_content_length = 0;
  std::size_t line_begin = request_line_end + 2;
  while (line_begin < header_end) {
    const std::size_t line_end = input.find("\r\n", line_begin);
    if (line_end == std::string_view::npos || line_end > header_end) {
      setError(HttpParseError::INVALID_HEADER,
               "HTTP header line is not terminated with CRLF");
      return 0;
    }

    const std::string_view line =
        input.substr(line_begin, line_end - line_begin);
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
      setError(HttpParseError::INVALID_HEADER,
               "empty or folded HTTP header line is not supported");
      return 0;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      setError(HttpParseError::INVALID_HEADER,
               "HTTP header line is missing ':'");
      return 0;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value =
        TrimOptionalWhitespace(line.substr(colon + 1));
    if (!IsValidHeaderName(name) || !IsValidHeaderValue(value)) {
      setError(HttpParseError::INVALID_HEADER,
               "HTTP header contains an invalid name or value");
      return 0;
    }

    if (CaseInsensitiveEqual(name, "content-length")) {
      u64 current_length = 0;
      if (content_length_seen || !ParseContentLength(value, current_length)) {
        setError(HttpParseError::INVALID_CONTENT_LENGTH,
                 "Content-Length must be one unsigned decimal value");
        return 0;
      }
      content_length_seen = true;
      parsed_content_length = current_length;
    } else if (CaseInsensitiveEqual(name, "transfer-encoding") &&
               !value.empty()) {
      setError(HttpParseError::UNSUPPORTED_TRANSFER_ENCODING,
               "Transfer-Encoding is not supported by the request parser yet");
      return 0;
    }

    request->setHeader(std::string(name), std::string(value));
    line_begin = line_end + 2;
  }

  if (parsed_content_length > kMaxBodySize) {
    setError(HttpParseError::BODY_TOO_LARGE,
             "HTTP request body exceeds the configured limit");
    return 0;
  }
  if (parsed_content_length >
      std::numeric_limits<std::size_t>::max() - header_size) {
    setError(HttpParseError::BODY_TOO_LARGE,
             "HTTP request size overflows size_t");
    return 0;
  }

  const std::size_t message_size =
      header_size + static_cast<std::size_t>(parsed_content_length);
  if (input.size() < message_size) {
    return 0;
  }

  request->setBody(std::string(input.substr(
      header_size, static_cast<std::size_t>(parsed_content_length))));
  request->initConnection();
  data_ = std::move(request);
  content_length_ = parsed_content_length;
  finished_ = true;
  return message_size;
}

void HttpRequestParser::setError(HttpParseError error, std::string message) {
  error_ = error;
  error_message_ = std::move(message);
  finished_ = false;
}

HttpResponseParser::HttpResponseParser(HttpMethod request_method)
    : data_(std::make_shared<HttpResponse>()), request_method_(request_method) {
}

std::size_t HttpResponseParser::execute(char *data, std::size_t length,
                                        bool eof) {
  if (data == nullptr || finished_ || hasError()) {
    return 0;
  }
  const std::size_t consumed = parse(std::string_view(data, length), eof);
  if (consumed != 0 && consumed < length) {
    std::memmove(data, data + consumed, length - consumed);
  }
  return consumed;
}

std::size_t HttpResponseParser::execute(std::string_view data, bool eof) {
  if (finished_ || hasError()) {
    return 0;
  }
  return parse(data, eof);
}

void HttpResponseParser::reset(HttpMethod request_method) {
  data_ = std::make_shared<HttpResponse>();
  request_method_ = request_method;
  error_ = HttpParseError::NONE;
  error_message_.clear();
  content_length_ = 0;
  chunked_ = false;
  close_delimited_ = false;
  finished_ = false;
}

std::size_t HttpResponseParser::parse(std::string_view input, bool eof) {
  const std::size_t header_end = input.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    if (input.size() > kMaxHeaderSize) {
      setError(HttpParseError::HEADER_TOO_LARGE,
               "HTTP response headers exceed the configured limit");
    }
    return 0;
  }

  const std::size_t header_size = header_end + 4;
  if (header_size > kMaxHeaderSize) {
    setError(HttpParseError::HEADER_TOO_LARGE,
             "HTTP response headers exceed the configured limit");
    return 0;
  }

  const std::size_t status_line_end = input.find("\r\n");
  if (status_line_end == std::string_view::npos ||
      status_line_end > header_end) {
    setError(HttpParseError::INVALID_STATUS, "HTTP status line is incomplete");
    return 0;
  }
  const std::string_view status_line = input.substr(0, status_line_end);
  if (status_line.size() < 12 || status_line[8] != ' ') {
    setError(HttpParseError::INVALID_STATUS,
             "HTTP status line must contain version and status code");
    return 0;
  }

  u8 version = 0;
  if (status_line.substr(0, 8) == "HTTP/1.1") {
    version = 0x11;
  } else if (status_line.substr(0, 8) == "HTTP/1.0") {
    version = 0x10;
  } else {
    setError(HttpParseError::INVALID_VERSION,
             "only HTTP/1.0 and HTTP/1.1 responses are supported");
    return 0;
  }

  const std::size_t reason_begin = status_line.find(' ', 9);
  const std::string_view status_text = status_line.substr(
      9, reason_begin == std::string_view::npos ? status_line.size() - 9
                                                : reason_begin - 9);
  if (status_text.size() != 3) {
    setError(HttpParseError::INVALID_STATUS,
             "HTTP status code must contain exactly three digits");
    return 0;
  }
  u16 status = 0;
  const auto [status_position, status_error] = std::from_chars(
      status_text.data(), status_text.data() + status_text.size(), status);
  if (status_error != std::errc{} ||
      status_position != status_text.data() + status_text.size() ||
      status < 100 || status > 999) {
    setError(HttpParseError::INVALID_STATUS, "invalid HTTP status code");
    return 0;
  }
  const std::string_view reason = reason_begin == std::string_view::npos
                                      ? std::string_view{}
                                      : status_line.substr(reason_begin + 1);
  if (!IsValidHeaderValue(reason)) {
    setError(HttpParseError::INVALID_STATUS,
             "HTTP reason phrase contains an invalid character");
    return 0;
  }

  auto response = std::make_shared<HttpResponse>(version, version <= 0x10);
  response->setStatus(static_cast<HttpStatus>(status));
  response->setReason(std::string(reason));

  bool content_length_seen = false;
  bool transfer_encoding_seen = false;
  bool connection_seen = false;
  u64 parsed_content_length = 0;
  std::string transfer_encoding;
  std::string connection_header;
  std::size_t line_begin = status_line_end + 2;
  while (line_begin < header_end) {
    const std::size_t line_end = input.find("\r\n", line_begin);
    if (line_end == std::string_view::npos || line_end > header_end) {
      setError(HttpParseError::INVALID_HEADER,
               "HTTP response header line is not terminated with CRLF");
      return 0;
    }
    const std::string_view line =
        input.substr(line_begin, line_end - line_begin);
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
      setError(HttpParseError::INVALID_HEADER,
               "empty or folded HTTP header line is not supported");
      return 0;
    }
    const std::size_t separator = line.find(':');
    if (separator == std::string_view::npos) {
      setError(HttpParseError::INVALID_HEADER,
               "HTTP response header line is missing ':'");
      return 0;
    }
    const std::string_view name = line.substr(0, separator);
    const std::string_view value =
        TrimOptionalWhitespace(line.substr(separator + 1));
    if (!IsValidHeaderName(name) || !IsValidHeaderValue(value)) {
      setError(HttpParseError::INVALID_HEADER,
               "HTTP response header contains an invalid name or value");
      return 0;
    }

    if (CaseInsensitiveEqual(name, "content-length")) {
      u64 current_length = 0;
      if (content_length_seen || !ParseContentLength(value, current_length)) {
        setError(HttpParseError::INVALID_CONTENT_LENGTH,
                 "Content-Length must be one unsigned decimal value");
        return 0;
      }
      content_length_seen = true;
      parsed_content_length = current_length;
    } else if (CaseInsensitiveEqual(name, "transfer-encoding")) {
      if (transfer_encoding_seen) {
        transfer_encoding.push_back(',');
      }
      transfer_encoding.append(value);
      transfer_encoding_seen = true;
    } else if (CaseInsensitiveEqual(name, "connection")) {
      if (connection_seen) {
        connection_header.push_back(',');
      }
      connection_header.append(value);
      connection_seen = true;
    }

    if (CaseInsensitiveEqual(name, "set-cookie")) {
      response->addCookie(std::string(value));
    } else if (!CaseInsensitiveEqual(name, "transfer-encoding") &&
               !CaseInsensitiveEqual(name, "connection")) {
      response->setHeader(std::string(name), std::string(value));
    }
    line_begin = line_end + 2;
  }

  if (content_length_seen && transfer_encoding_seen) {
    setError(HttpParseError::INVALID_CONTENT_LENGTH,
             "Content-Length and Transfer-Encoding cannot be combined");
    return 0;
  }
  if (transfer_encoding_seen) {
    response->setHeader("Transfer-Encoding", transfer_encoding);
  }
  if (connection_seen) {
    response->setHeader("Connection", connection_header);
  }
  if (parsed_content_length > kMaxBodySize) {
    setError(HttpParseError::BODY_TOO_LARGE,
             "HTTP response body exceeds the configured limit");
    return 0;
  }

  response->initConnection();
  if (ResponseHasNoBody(request_method_, status)) {
    data_ = std::move(response);
    content_length_ = 0;
    finished_ = true;
    return header_size;
  }

  if (transfer_encoding_seen) {
    if (!HeaderContainsFinalChunked(transfer_encoding)) {
      setError(HttpParseError::UNSUPPORTED_TRANSFER_ENCODING,
               "the final Transfer-Encoding must be chunked");
      return 0;
    }

    std::string body;
    std::size_t position = header_size;
    while (true) {
      const std::size_t chunk_line_end = input.find("\r\n", position);
      if (chunk_line_end == std::string_view::npos) {
        if (input.size() - position > kMaxHeaderSize) {
          setError(HttpParseError::HEADER_TOO_LARGE,
                   "HTTP chunk metadata exceeds the configured limit");
        }
        return 0;
      }
      u64 chunk_size = 0;
      if (!ParseChunkSize(input.substr(position, chunk_line_end - position),
                          chunk_size)) {
        setError(HttpParseError::INVALID_CHUNK_SIZE, "invalid HTTP chunk size");
        return 0;
      }
      position = chunk_line_end + 2;

      if (chunk_size == 0) {
        const std::size_t trailers_begin = position;
        while (true) {
          const std::size_t trailer_end = input.find("\r\n", position);
          if (trailer_end == std::string_view::npos) {
            if (input.size() - trailers_begin > kMaxHeaderSize) {
              setError(HttpParseError::HEADER_TOO_LARGE,
                       "HTTP trailers exceed the configured limit");
            }
            return 0;
          }
          if (trailer_end - trailers_begin > kMaxHeaderSize) {
            setError(HttpParseError::HEADER_TOO_LARGE,
                     "HTTP trailers exceed the configured limit");
            return 0;
          }
          const std::string_view trailer =
              input.substr(position, trailer_end - position);
          position = trailer_end + 2;
          if (trailer.empty()) {
            response->setBody(std::move(body));
            response->initConnection();
            data_ = std::move(response);
            content_length_ = data_->getBody().size();
            chunked_ = true;
            finished_ = true;
            return position;
          }
          const std::size_t separator = trailer.find(':');
          if (separator == std::string_view::npos) {
            setError(HttpParseError::INVALID_HEADER,
                     "HTTP trailer line is missing ':'");
            return 0;
          }
          const std::string_view name = trailer.substr(0, separator);
          const std::string_view value =
              TrimOptionalWhitespace(trailer.substr(separator + 1));
          if (!IsValidHeaderName(name) || !IsValidHeaderValue(value)) {
            setError(HttpParseError::INVALID_HEADER,
                     "HTTP trailer contains an invalid name or value");
            return 0;
          }
          if (!CaseInsensitiveEqual(name, "content-length") &&
              !CaseInsensitiveEqual(name, "transfer-encoding")) {
            response->setHeader(std::string(name), std::string(value));
          }
        }
      }

      if (chunk_size > kMaxBodySize - body.size()) {
        setError(HttpParseError::BODY_TOO_LARGE,
                 "HTTP chunked body exceeds the configured limit");
        return 0;
      }
      if (chunk_size > std::numeric_limits<std::size_t>::max() - position - 2) {
        setError(HttpParseError::BODY_TOO_LARGE,
                 "HTTP chunk size overflows size_t");
        return 0;
      }
      const std::size_t data_end =
          position + static_cast<std::size_t>(chunk_size);
      if (input.size() < data_end + 2) {
        return 0;
      }
      if (input.substr(data_end, 2) != "\r\n") {
        setError(HttpParseError::INVALID_CHUNK_SIZE,
                 "HTTP chunk data is not followed by CRLF");
        return 0;
      }
      body.append(input.substr(position, static_cast<std::size_t>(chunk_size)));
      position = data_end + 2;
    }
  }

  if (content_length_seen) {
    if (parsed_content_length >
        std::numeric_limits<std::size_t>::max() - header_size) {
      setError(HttpParseError::BODY_TOO_LARGE,
               "HTTP response size overflows size_t");
      return 0;
    }
    const std::size_t message_size =
        header_size + static_cast<std::size_t>(parsed_content_length);
    if (input.size() < message_size) {
      return 0;
    }
    response->setBody(std::string(input.substr(
        header_size, static_cast<std::size_t>(parsed_content_length))));
    data_ = std::move(response);
    content_length_ = parsed_content_length;
    finished_ = true;
    return message_size;
  }

  close_delimited_ = true;
  response->setClose(true);
  if (input.size() - header_size > kMaxBodySize) {
    setError(HttpParseError::BODY_TOO_LARGE,
             "HTTP close-delimited body exceeds the configured limit");
    return 0;
  }
  if (!eof) {
    return 0;
  }
  response->setBody(std::string(input.substr(header_size)));
  data_ = std::move(response);
  content_length_ = data_->getBody().size();
  finished_ = true;
  return input.size();
}

void HttpResponseParser::setError(HttpParseError error, std::string message) {
  error_ = error;
  error_message_ = std::move(message);
  finished_ = false;
}

const char *HttpParseErrorToString(HttpParseError error) noexcept {
  switch (error) {
  case HttpParseError::NONE:
    return "none";
  case HttpParseError::INVALID_METHOD:
    return "invalid method";
  case HttpParseError::INVALID_REQUEST_TARGET:
    return "invalid request target";
  case HttpParseError::INVALID_VERSION:
    return "invalid HTTP version";
  case HttpParseError::INVALID_STATUS:
    return "invalid HTTP status";
  case HttpParseError::INVALID_HEADER:
    return "invalid header";
  case HttpParseError::INVALID_CONTENT_LENGTH:
    return "invalid Content-Length";
  case HttpParseError::INVALID_CHUNK_SIZE:
    return "invalid chunk size";
  case HttpParseError::UNSUPPORTED_TRANSFER_ENCODING:
    return "unsupported Transfer-Encoding";
  case HttpParseError::HEADER_TOO_LARGE:
    return "headers too large";
  case HttpParseError::BODY_TOO_LARGE:
    return "body too large";
  }
  return "unknown parse error";
}

} // namespace droplet::http
