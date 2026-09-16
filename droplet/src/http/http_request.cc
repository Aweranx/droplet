#include <droplet/http/http_request.h>
#include <droplet/types.h>

#include <droplet/http/http_response.h>

#include <algorithm>
#include <sstream>
#include <utility>

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

[[nodiscard]] int HexValue(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  const unsigned char lowered = AsciiLower(static_cast<unsigned char>(value));
  if (lowered >= 'a' && lowered <= 'f') {
    return lowered - 'a' + 10;
  }
  return -1;
}

[[nodiscard]] std::string UrlDecode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      result.push_back(' ');
      continue;
    }
    if (value[index] == '%' && index + 2 < value.size()) {
      const int high = HexValue(value[index + 1]);
      const int low = HexValue(value[index + 2]);
      if (high >= 0 && low >= 0) {
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    result.push_back(value[index]);
  }
  return result;
}

void ParseKeyValueList(std::string_view input, char separator, HttpMap &output,
                       bool trim_parts) {
  std::size_t begin = 0;
  while (begin <= input.size()) {
    const std::size_t end = input.find(separator, begin);
    std::string_view part =
        input.substr(begin, end == std::string_view::npos ? input.size() - begin
                                                          : end - begin);
    if (trim_parts) {
      part = Trim(part);
    }
    if (!part.empty()) {
      const std::size_t equal = part.find('=');
      std::string_view key =
          equal == std::string_view::npos ? part : part.substr(0, equal);
      std::string_view value = equal == std::string_view::npos
                                   ? std::string_view{}
                                   : part.substr(equal + 1);
      if (trim_parts) {
        key = Trim(key);
        value = Trim(value);
      }
      if (!key.empty()) {
        output.try_emplace(UrlDecode(key), UrlDecode(value));
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
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

[[nodiscard]] bool ShouldSkipHeader(std::string_view name, bool websocket,
                                    bool has_body) noexcept {
  return (!websocket && CaseInsensitiveEqual(name, "connection")) ||
         (has_body && CaseInsensitiveEqual(name, "content-length"));
}

} // namespace

HttpRequest::HttpRequest(u8 version, bool close)
    : version_(version), close_(close) {}

std::shared_ptr<HttpResponse> HttpRequest::createResponse() const {
  return std::make_shared<HttpResponse>(version_, close_);
}

const HttpRequest::MapType &HttpRequest::getParams() const {
  initParams();
  return params_;
}

const HttpRequest::MapType &HttpRequest::getCookies() const {
  initCookies();
  return cookies_;
}

void HttpRequest::setQuery(std::string value) {
  query_ = std::move(value);
  params_.clear();
  params_initialized_ = false;
}

void HttpRequest::setBody(std::string value) {
  body_ = std::move(value);
  params_.clear();
  params_initialized_ = false;
}

void HttpRequest::setHeaders(MapType value) {
  headers_ = std::move(value);
  params_.clear();
  cookies_.clear();
  params_initialized_ = false;
  cookies_initialized_ = false;
}

void HttpRequest::setParams(MapType value) {
  params_ = std::move(value);
  params_initialized_ = true;
}

void HttpRequest::setCookies(MapType value) {
  cookies_ = std::move(value);
  cookies_initialized_ = true;
}

std::string HttpRequest::getHeader(std::string_view key,
                                   std::string default_value) const {
  const auto iterator = headers_.find(key);
  return iterator == headers_.end() ? std::move(default_value)
                                    : iterator->second;
}

std::string HttpRequest::getParam(std::string_view key,
                                  std::string default_value) const {
  initParams();
  const auto iterator = params_.find(key);
  return iterator == params_.end() ? std::move(default_value)
                                   : iterator->second;
}

std::string HttpRequest::getCookie(std::string_view key,
                                   std::string default_value) const {
  initCookies();
  const auto iterator = cookies_.find(key);
  return iterator == cookies_.end() ? std::move(default_value)
                                    : iterator->second;
}

void HttpRequest::setHeader(std::string key, std::string value) {
  const bool affects_params = CaseInsensitiveEqual(key, "content-type");
  const bool affects_cookies = CaseInsensitiveEqual(key, "cookie");
  headers_.insert_or_assign(std::move(key), std::move(value));
  if (affects_params) {
    params_.clear();
    params_initialized_ = false;
  }
  if (affects_cookies) {
    cookies_.clear();
    cookies_initialized_ = false;
  }
}

void HttpRequest::setParam(std::string key, std::string value) {
  initParams();
  params_.insert_or_assign(std::move(key), std::move(value));
}

void HttpRequest::setCookie(std::string key, std::string value) {
  initCookies();
  cookies_.insert_or_assign(std::move(key), std::move(value));
}

void HttpRequest::delHeader(std::string_view key) {
  const bool affects_params = CaseInsensitiveEqual(key, "content-type");
  const bool affects_cookies = CaseInsensitiveEqual(key, "cookie");
  if (const auto iterator = headers_.find(key); iterator != headers_.end()) {
    headers_.erase(iterator);
  }
  if (affects_params) {
    params_.clear();
    params_initialized_ = false;
  }
  if (affects_cookies) {
    cookies_.clear();
    cookies_initialized_ = false;
  }
}

void HttpRequest::delParam(std::string_view key) {
  initParams();
  if (const auto iterator = params_.find(key); iterator != params_.end()) {
    params_.erase(iterator);
  }
}

void HttpRequest::delCookie(std::string_view key) {
  initCookies();
  if (const auto iterator = cookies_.find(key); iterator != cookies_.end()) {
    cookies_.erase(iterator);
  }
}

bool HttpRequest::hasHeader(std::string_view key, std::string *value) const {
  const auto iterator = headers_.find(key);
  if (iterator == headers_.end()) {
    return false;
  }
  if (value != nullptr) {
    *value = iterator->second;
  }
  return true;
}

bool HttpRequest::hasParam(std::string_view key, std::string *value) const {
  initParams();
  const auto iterator = params_.find(key);
  if (iterator == params_.end()) {
    return false;
  }
  if (value != nullptr) {
    *value = iterator->second;
  }
  return true;
}

bool HttpRequest::hasCookie(std::string_view key, std::string *value) const {
  initCookies();
  const auto iterator = cookies_.find(key);
  if (iterator == cookies_.end()) {
    return false;
  }
  if (value != nullptr) {
    *value = iterator->second;
  }
  return true;
}

void HttpRequest::initConnection() {
  close_ = version_ <= 0x10;
  const std::string connection = getHeader("connection");
  if (HeaderContainsToken(connection, "close")) {
    close_ = true;
  } else if (HeaderContainsToken(connection, "keep-alive")) {
    close_ = false;
  }
}

void HttpRequest::initParams() const {
  if (params_initialized_) {
    return;
  }
  params_.clear();
  ParseKeyValueList(query_, '&', params_, false);

  const std::string content_type_header = getHeader("content-type");
  std::string_view content_type = content_type_header;
  const std::size_t semicolon = content_type.find(';');
  content_type = Trim(content_type.substr(0, semicolon));
  if (CaseInsensitiveEqual(content_type, "application/x-www-form-urlencoded")) {
    ParseKeyValueList(body_, '&', params_, false);
  }
  params_initialized_ = true;
}

void HttpRequest::initCookies() const {
  if (cookies_initialized_) {
    return;
  }
  cookies_.clear();
  ParseKeyValueList(getHeader("cookie"), ';', cookies_, true);
  cookies_initialized_ = true;
}

std::ostream &HttpRequest::dump(std::ostream &stream) const {
  stream << HttpMethodToString(method_) << ' ' << (path_.empty() ? "/" : path_);
  if (!query_.empty()) {
    stream << '?' << query_;
  }
  if (!fragment_.empty()) {
    stream << '#' << fragment_;
  }
  stream << " HTTP/" << static_cast<unsigned int>(version_ >> 4U) << '.'
         << static_cast<unsigned int>(version_ & 0x0FU) << "\r\n";

  for (const auto &[name, value] : headers_) {
    if (!ShouldSkipHeader(name, websocket_, !body_.empty())) {
      stream << name << ": " << value << "\r\n";
    }
  }
  if (!websocket_) {
    stream << "Connection: " << (close_ ? "close" : "keep-alive") << "\r\n";
  }
  if (!body_.empty()) {
    stream << "Content-Length: " << body_.size() << "\r\n";
  }
  stream << "\r\n" << body_;
  return stream;
}

std::string HttpRequest::toString() const {
  std::ostringstream stream;
  dump(stream);
  return stream.str();
}

std::ostream &operator<<(std::ostream &stream, const HttpRequest &request) {
  return request.dump(stream);
}

} // namespace droplet::http
