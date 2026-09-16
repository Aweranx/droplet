#pragma once

#include <droplet/types.h>

#include <droplet/export.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace droplet::http {

/** @brief HTTP 请求方法。 */
enum class HttpMethod : u8 {
  DELETE = 0,
  GET,
  HEAD,
  POST,
  PUT,
  CONNECT,
  OPTIONS,
  TRACE,
  COPY,
  LOCK,
  MKCOL,
  MOVE,
  PROPFIND,
  PROPPATCH,
  SEARCH,
  UNLOCK,
  BIND,
  REBIND,
  UNBIND,
  ACL,
  REPORT,
  MKACTIVITY,
  CHECKOUT,
  MERGE,
  MSEARCH,
  NOTIFY,
  SUBSCRIBE,
  UNSUBSCRIBE,
  PATCH,
  PURGE,
  MKCALENDAR,
  LINK,
  UNLINK,
  SOURCE,
  INVALID_METHOD,
};

/** @brief 常用 HTTP 响应状态码。 */
enum class HttpStatus : u16 {
  CONTINUE = 100,
  SWITCHING_PROTOCOLS = 101,
  PROCESSING = 102,
  EARLY_HINTS = 103,

  OK = 200,
  CREATED = 201,
  ACCEPTED = 202,
  NON_AUTHORITATIVE_INFORMATION = 203,
  NO_CONTENT = 204,
  RESET_CONTENT = 205,
  PARTIAL_CONTENT = 206,
  MULTI_STATUS = 207,
  ALREADY_REPORTED = 208,
  IM_USED = 226,

  MULTIPLE_CHOICES = 300,
  MOVED_PERMANENTLY = 301,
  FOUND = 302,
  SEE_OTHER = 303,
  NOT_MODIFIED = 304,
  USE_PROXY = 305,
  TEMPORARY_REDIRECT = 307,
  PERMANENT_REDIRECT = 308,

  BAD_REQUEST = 400,
  UNAUTHORIZED = 401,
  PAYMENT_REQUIRED = 402,
  FORBIDDEN = 403,
  NOT_FOUND = 404,
  METHOD_NOT_ALLOWED = 405,
  NOT_ACCEPTABLE = 406,
  PROXY_AUTHENTICATION_REQUIRED = 407,
  REQUEST_TIMEOUT = 408,
  CONFLICT = 409,
  GONE = 410,
  LENGTH_REQUIRED = 411,
  PRECONDITION_FAILED = 412,
  CONTENT_TOO_LARGE = 413,
  PAYLOAD_TOO_LARGE = CONTENT_TOO_LARGE,
  URI_TOO_LONG = 414,
  UNSUPPORTED_MEDIA_TYPE = 415,
  RANGE_NOT_SATISFIABLE = 416,
  EXPECTATION_FAILED = 417,
  IM_A_TEAPOT = 418,
  MISDIRECTED_REQUEST = 421,
  UNPROCESSABLE_CONTENT = 422,
  UNPROCESSABLE_ENTITY = UNPROCESSABLE_CONTENT,
  LOCKED = 423,
  FAILED_DEPENDENCY = 424,
  TOO_EARLY = 425,
  UPGRADE_REQUIRED = 426,
  PRECONDITION_REQUIRED = 428,
  TOO_MANY_REQUESTS = 429,
  REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
  UNAVAILABLE_FOR_LEGAL_REASONS = 451,

  INTERNAL_SERVER_ERROR = 500,
  NOT_IMPLEMENTED = 501,
  BAD_GATEWAY = 502,
  SERVICE_UNAVAILABLE = 503,
  GATEWAY_TIMEOUT = 504,
  HTTP_VERSION_NOT_SUPPORTED = 505,
  VARIANT_ALSO_NEGOTIATES = 506,
  INSUFFICIENT_STORAGE = 507,
  LOOP_DETECTED = 508,
  NOT_EXTENDED = 510,
  NETWORK_AUTHENTICATION_REQUIRED = 511,
};

[[nodiscard]] DROPLET_API HttpMethod
StringToHttpMethod(std::string_view method) noexcept;
[[nodiscard]] DROPLET_API HttpMethod
CharsToHttpMethod(const char *method) noexcept;
[[nodiscard]] DROPLET_API const char *
HttpMethodToString(HttpMethod method) noexcept;
[[nodiscard]] DROPLET_API const char *
HttpStatusToString(HttpStatus status) noexcept;

/** @brief 让 HTTP 头字段名按 ASCII 大小写不敏感规则排序。 */
struct DROPLET_API CaseInsensitiveLess {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs,
                                std::string_view rhs) const noexcept;
};

using HttpMap = std::map<std::string, std::string, CaseInsensitiveLess>;

namespace detail {

template <class T>
bool CheckGetAs(const HttpMap &values, std::string_view key, T &value,
                const T &default_value = T{}) {
  const auto iterator = values.find(key);
  if (iterator == values.end()) {
    value = default_value;
    return false;
  }

  if constexpr (std::is_same_v<T, std::string>) {
    value = iterator->second;
    return true;
  } else {
    std::istringstream stream(iterator->second);
    T converted{};
    stream >> std::boolalpha >> converted;
    if (stream.fail()) {
      value = default_value;
      return false;
    }
    if (!stream.eof()) {
      stream >> std::ws;
      if (stream.peek() != std::char_traits<char>::eof()) {
        value = default_value;
        return false;
      }
    }
    value = std::move(converted);
    return true;
  }
}

template <class T>
T GetAs(const HttpMap &values, std::string_view key,
        const T &default_value = T{}) {
  T value{};
  CheckGetAs(values, key, value, default_value);
  return value;
}

} // namespace detail

} // namespace droplet::http
