#include <droplet/http/http_response.h>
#include <droplet/types.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace droplet::http {
namespace {

[[nodiscard]] constexpr unsigned char AsciiLower(unsigned char value) noexcept {
  return value >= 'A' && value <= 'Z'
             ? static_cast<unsigned char>(value + ('a' - 'A'))
             : value;
}

[[nodiscard]] bool CaseInsensitiveEqual(std::string_view lhs,
                                        std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](char left, char right) {
                      return AsciiLower(static_cast<unsigned char>(left)) ==
                             AsciiLower(static_cast<unsigned char>(right));
                    });
}

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool HeaderContainsToken(std::string_view header,
                                       std::string_view token) noexcept {
  std::size_t begin = 0;
  while (begin <= header.size()) {
    const std::size_t end = header.find(',', begin);
    const std::string_view item = Trim(header.substr(
        begin,
        end == std::string_view::npos ? header.size() - begin : end - begin));
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

[[nodiscard]] std::string FormatHttpDate(std::time_t value) {
  std::tm broken_down{};
#if defined(_WIN32)
  gmtime_s(&broken_down, &value);
#else
  gmtime_r(&value, &broken_down);
#endif
  std::array<char, 64> buffer{};
  const std::size_t length = std::strftime(
      buffer.data(), buffer.size(), "%a, %d %b %Y %H:%M:%S GMT", &broken_down);
  return std::string(buffer.data(), length);
}

[[nodiscard]] bool ResponseAllowsBody(HttpStatus status) noexcept {
  const auto code = static_cast<u16>(status);
  return !(code >= 100 && code < 200) && status != HttpStatus::NO_CONTENT &&
         status != HttpStatus::NOT_MODIFIED;
}

} // namespace

HttpResponse::HttpResponse(u8 version, bool close)
    : version_(version), close_(close) {}

std::string HttpResponse::getHeader(std::string_view key,
                                    std::string default_value) const {
  const auto iterator = headers_.find(key);
  return iterator == headers_.end() ? std::move(default_value)
                                    : iterator->second;
}

void HttpResponse::setHeader(std::string key, std::string value) {
  headers_.insert_or_assign(std::move(key), std::move(value));
}

void HttpResponse::delHeader(std::string_view key) {
  if (const auto iterator = headers_.find(key); iterator != headers_.end()) {
    headers_.erase(iterator);
  }
}

bool HttpResponse::hasHeader(std::string_view key, std::string *value) const {
  const auto iterator = headers_.find(key);
  if (iterator == headers_.end()) {
    return false;
  }
  if (value != nullptr) {
    *value = iterator->second;
  }
  return true;
}

void HttpResponse::setRedirect(std::string uri) {
  status_ = HttpStatus::FOUND;
  setHeader("Location", std::move(uri));
}

void HttpResponse::setCookie(std::string key, std::string value,
                             std::time_t expires, std::string path,
                             std::string domain, bool secure, bool http_only,
                             std::string same_site) {
  std::ostringstream cookie;
  cookie << key << '=' << value;
  if (expires > 0) {
    cookie << "; Expires=" << FormatHttpDate(expires);
  }
  if (!path.empty()) {
    cookie << "; Path=" << path;
  }
  if (!domain.empty()) {
    cookie << "; Domain=" << domain;
  }
  if (secure) {
    cookie << "; Secure";
  }
  if (http_only) {
    cookie << "; HttpOnly";
  }
  if (!same_site.empty()) {
    cookie << "; SameSite=" << same_site;
  }
  cookies_.push_back(cookie.str());
}

void HttpResponse::initConnection() {
  close_ = version_ <= 0x10;
  const std::string connection = getHeader("connection");
  if (HeaderContainsToken(connection, "close")) {
    close_ = true;
  } else if (HeaderContainsToken(connection, "keep-alive")) {
    close_ = false;
  }
}

std::ostream &HttpResponse::dump(std::ostream &stream) const {
  stream << "HTTP/" << static_cast<unsigned int>(version_ >> 4U) << '.'
         << static_cast<unsigned int>(version_ & 0x0FU) << ' '
         << static_cast<u16>(status_) << ' '
         << (reason_.empty() ? HttpStatusToString(status_) : reason_) << "\r\n";

  const bool allows_body = ResponseAllowsBody(status_);
  for (const auto &[name, value] : headers_) {
    if ((!websocket_ && CaseInsensitiveEqual(name, "connection")) ||
        (!websocket_ && CaseInsensitiveEqual(name, "content-length"))) {
      continue;
    }
    stream << name << ": " << value << "\r\n";
  }
  for (const std::string &cookie : cookies_) {
    stream << "Set-Cookie: " << cookie << "\r\n";
  }
  if (!websocket_) {
    stream << "Connection: " << (close_ ? "close" : "keep-alive") << "\r\n";
    if (allows_body) {
      stream << "Content-Length: " << body_.size() << "\r\n";
    }
  }
  stream << "\r\n";
  if (allows_body) {
    stream << body_;
  }
  return stream;
}

std::string HttpResponse::toString() const {
  std::ostringstream stream;
  dump(stream);
  return stream.str();
}

std::ostream &operator<<(std::ostream &stream, const HttpResponse &response) {
  return response.dump(stream);
}

} // namespace droplet::http
