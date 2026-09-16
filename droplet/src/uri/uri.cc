#include <droplet/types.h>
#include <droplet/uri/uri.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <system_error>

#include <sys/socket.h>

namespace droplet {
namespace {

[[nodiscard]] bool IsSchemeStart(unsigned char value) noexcept {
  return std::isalpha(value) != 0;
}

[[nodiscard]] bool IsSchemeCharacter(unsigned char value) noexcept {
  return std::isalnum(value) != 0 || value == '+' || value == '-' ||
         value == '.';
}

[[nodiscard]] bool IsHexDigit(unsigned char value) noexcept {
  return std::isxdigit(value) != 0;
}

[[nodiscard]] bool IsValidUriText(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size(); ++index) {
    const unsigned char value = static_cast<unsigned char>(text[index]);
    if (value <= 0x20 || value == 0x7f) {
      return false;
    }
    if (value == '%') {
      if (index + 2 >= text.size() ||
          !IsHexDigit(static_cast<unsigned char>(text[index + 1])) ||
          !IsHexDigit(static_cast<unsigned char>(text[index + 2]))) {
        return false;
      }
      index += 2;
    }
  }
  return true;
}

[[nodiscard]] bool ParsePort(std::string_view text, i32 &port) noexcept {
  if (text.empty()) {
    return false;
  }
  u32 value = 0;
  const auto [position, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || position != text.data() + text.size() ||
      value > std::numeric_limits<u16>::max()) {
    return false;
  }
  port = static_cast<i32>(value);
  return true;
}

void ToLowerAscii(std::string &value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    const unsigned char byte = static_cast<unsigned char>(character);
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A'))
                                      : character;
  });
}

[[nodiscard]] bool ParseAuthority(std::string_view authority, Uri &uri) {
  const std::size_t at = authority.rfind('@');
  std::string_view host_port = authority;
  if (at != std::string_view::npos) {
    uri.setUserinfo(std::string(authority.substr(0, at)));
    host_port.remove_prefix(at + 1);
  }

  if (host_port.starts_with('[')) {
    const std::size_t closing = host_port.find(']');
    if (closing == std::string_view::npos || closing == 1) {
      return false;
    }
    uri.setHost(std::string(host_port.substr(1, closing - 1)));
    const std::string_view suffix = host_port.substr(closing + 1);
    if (suffix.empty()) {
      return true;
    }
    if (!suffix.starts_with(':')) {
      return false;
    }
    i32 port = -1;
    if (!ParsePort(suffix.substr(1), port)) {
      return false;
    }
    uri.setPort(port);
    return true;
  }

  if (host_port.find('[') != std::string_view::npos ||
      host_port.find(']') != std::string_view::npos) {
    return false;
  }
  const std::size_t colon = host_port.rfind(':');
  if (colon != std::string_view::npos) {
    // IPv6 字面量必须使用 [::1]，否则无法区分地址冒号和端口冒号。
    if (host_port.find(':') != colon) {
      return false;
    }
    i32 port = -1;
    if (!ParsePort(host_port.substr(colon + 1), port)) {
      return false;
    }
    uri.setPort(port);
    host_port = host_port.substr(0, colon);
  }
  uri.setHost(std::string(host_port));
  return true;
}

} // namespace

Uri::Ptr Uri::Create(std::string_view text) {
  if (!IsValidUriText(text)) {
    return nullptr;
  }

  auto uri = std::make_shared<Uri>();
  std::string_view hierarchy = text;

  const std::size_t fragment = hierarchy.find('#');
  if (fragment != std::string_view::npos) {
    uri->setFragment(std::string(hierarchy.substr(fragment + 1)));
    hierarchy = hierarchy.substr(0, fragment);
  }
  const std::size_t query = hierarchy.find('?');
  if (query != std::string_view::npos) {
    uri->setQuery(std::string(hierarchy.substr(query + 1)));
    hierarchy = hierarchy.substr(0, query);
  }

  const std::size_t colon = hierarchy.find(':');
  const std::size_t slash = hierarchy.find('/');
  if (colon != std::string_view::npos &&
      (slash == std::string_view::npos || colon < slash)) {
    const std::string_view scheme = hierarchy.substr(0, colon);
    if (scheme.empty() ||
        !IsSchemeStart(static_cast<unsigned char>(scheme.front())) ||
        !std::all_of(scheme.begin() + 1, scheme.end(), [](char character) {
          return IsSchemeCharacter(static_cast<unsigned char>(character));
        })) {
      return nullptr;
    }
    uri->setScheme(std::string(scheme));
    hierarchy.remove_prefix(colon + 1);
  }

  if (hierarchy.starts_with("//")) {
    hierarchy.remove_prefix(2);
    const std::size_t path_begin = hierarchy.find('/');
    const std::string_view authority = hierarchy.substr(0, path_begin);
    if (!ParseAuthority(authority, *uri)) {
      return nullptr;
    }
    hierarchy = path_begin == std::string_view::npos
                    ? std::string_view{}
                    : hierarchy.substr(path_begin);
  }

  uri->setPath(std::string(hierarchy));
  return uri;
}

const std::string &Uri::getPath() const noexcept {
  static const std::string default_path = "/";
  return path_.empty() ? default_path : path_;
}

i32 Uri::getPort() const noexcept {
  if (port_ >= 0) {
    return port_;
  }
  if (scheme_ == "http" || scheme_ == "ws") {
    return 80;
  }
  if (scheme_ == "https" || scheme_ == "wss") {
    return 443;
  }
  return -1;
}

bool Uri::isDefaultPort() const noexcept {
  if (port_ < 0) {
    return true;
  }
  return ((scheme_ == "http" || scheme_ == "ws") && port_ == 80) ||
         ((scheme_ == "https" || scheme_ == "wss") && port_ == 443);
}

void Uri::setScheme(std::string value) {
  ToLowerAscii(value);
  scheme_ = std::move(value);
}

std::ostream &Uri::dump(std::ostream &stream) const {
  if (!scheme_.empty()) {
    stream << scheme_ << ':';
  }
  if (has_authority_) {
    stream << "//";
    if (!userinfo_.empty()) {
      stream << userinfo_ << '@';
    }
    if (host_.find(':') != std::string::npos) {
      stream << '[' << host_ << ']';
    } else {
      stream << host_;
    }
    if (port_ >= 0 && !isDefaultPort()) {
      stream << ':' << port_;
    }
  }
  if (path_.empty()) {
    if (has_authority_) {
      stream << '/';
    }
  } else {
    stream << path_;
  }
  if (!query_.empty()) {
    stream << '?' << query_;
  }
  if (!fragment_.empty()) {
    stream << '#' << fragment_;
  }
  return stream;
}

std::string Uri::toString() const {
  std::ostringstream stream;
  dump(stream);
  return stream.str();
}

IPAddress::Ptr Uri::createAddress() const {
  if (host_.empty()) {
    return nullptr;
  }
  auto address = Address::LookupAnyIPAddress(host_, AF_UNSPEC, SOCK_STREAM);
  const i32 port = getPort();
  if (!address || port < 0 || port > std::numeric_limits<u16>::max()) {
    return nullptr;
  }
  address->setPort(static_cast<u16>(port));
  return address;
}

std::ostream &operator<<(std::ostream &stream, const Uri &uri) {
  return uri.dump(stream);
}

} // namespace droplet
