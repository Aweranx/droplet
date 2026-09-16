#include <droplet/http/http.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace droplet::http {
namespace {

constexpr std::array<std::string_view, 34> kMethodNames{
    "DELETE",     "GET",        "HEAD",        "POST",      "PUT",
    "CONNECT",    "OPTIONS",    "TRACE",       "COPY",      "LOCK",
    "MKCOL",      "MOVE",       "PROPFIND",    "PROPPATCH", "SEARCH",
    "UNLOCK",     "BIND",       "REBIND",      "UNBIND",    "ACL",
    "REPORT",     "MKACTIVITY", "CHECKOUT",    "MERGE",     "M-SEARCH",
    "NOTIFY",     "SUBSCRIBE",  "UNSUBSCRIBE", "PATCH",     "PURGE",
    "MKCALENDAR", "LINK",       "UNLINK",      "SOURCE",
};

constexpr std::pair<HttpStatus, const char *> kStatusNames[]{
    {HttpStatus::CONTINUE, "Continue"},
    {HttpStatus::SWITCHING_PROTOCOLS, "Switching Protocols"},
    {HttpStatus::PROCESSING, "Processing"},
    {HttpStatus::EARLY_HINTS, "Early Hints"},
    {HttpStatus::OK, "OK"},
    {HttpStatus::CREATED, "Created"},
    {HttpStatus::ACCEPTED, "Accepted"},
    {HttpStatus::NON_AUTHORITATIVE_INFORMATION,
     "Non-Authoritative Information"},
    {HttpStatus::NO_CONTENT, "No Content"},
    {HttpStatus::RESET_CONTENT, "Reset Content"},
    {HttpStatus::PARTIAL_CONTENT, "Partial Content"},
    {HttpStatus::MULTI_STATUS, "Multi-Status"},
    {HttpStatus::ALREADY_REPORTED, "Already Reported"},
    {HttpStatus::IM_USED, "IM Used"},
    {HttpStatus::MULTIPLE_CHOICES, "Multiple Choices"},
    {HttpStatus::MOVED_PERMANENTLY, "Moved Permanently"},
    {HttpStatus::FOUND, "Found"},
    {HttpStatus::SEE_OTHER, "See Other"},
    {HttpStatus::NOT_MODIFIED, "Not Modified"},
    {HttpStatus::USE_PROXY, "Use Proxy"},
    {HttpStatus::TEMPORARY_REDIRECT, "Temporary Redirect"},
    {HttpStatus::PERMANENT_REDIRECT, "Permanent Redirect"},
    {HttpStatus::BAD_REQUEST, "Bad Request"},
    {HttpStatus::UNAUTHORIZED, "Unauthorized"},
    {HttpStatus::PAYMENT_REQUIRED, "Payment Required"},
    {HttpStatus::FORBIDDEN, "Forbidden"},
    {HttpStatus::NOT_FOUND, "Not Found"},
    {HttpStatus::METHOD_NOT_ALLOWED, "Method Not Allowed"},
    {HttpStatus::NOT_ACCEPTABLE, "Not Acceptable"},
    {HttpStatus::PROXY_AUTHENTICATION_REQUIRED,
     "Proxy Authentication Required"},
    {HttpStatus::REQUEST_TIMEOUT, "Request Timeout"},
    {HttpStatus::CONFLICT, "Conflict"},
    {HttpStatus::GONE, "Gone"},
    {HttpStatus::LENGTH_REQUIRED, "Length Required"},
    {HttpStatus::PRECONDITION_FAILED, "Precondition Failed"},
    {HttpStatus::CONTENT_TOO_LARGE, "Payload Too Large"},
    {HttpStatus::URI_TOO_LONG, "URI Too Long"},
    {HttpStatus::UNSUPPORTED_MEDIA_TYPE, "Unsupported Media Type"},
    {HttpStatus::RANGE_NOT_SATISFIABLE, "Range Not Satisfiable"},
    {HttpStatus::EXPECTATION_FAILED, "Expectation Failed"},
    {HttpStatus::IM_A_TEAPOT, "I'm a Teapot"},
    {HttpStatus::MISDIRECTED_REQUEST, "Misdirected Request"},
    {HttpStatus::UNPROCESSABLE_CONTENT, "Unprocessable Entity"},
    {HttpStatus::LOCKED, "Locked"},
    {HttpStatus::FAILED_DEPENDENCY, "Failed Dependency"},
    {HttpStatus::TOO_EARLY, "Too Early"},
    {HttpStatus::UPGRADE_REQUIRED, "Upgrade Required"},
    {HttpStatus::PRECONDITION_REQUIRED, "Precondition Required"},
    {HttpStatus::TOO_MANY_REQUESTS, "Too Many Requests"},
    {HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE,
     "Request Header Fields Too Large"},
    {HttpStatus::UNAVAILABLE_FOR_LEGAL_REASONS,
     "Unavailable For Legal Reasons"},
    {HttpStatus::INTERNAL_SERVER_ERROR, "Internal Server Error"},
    {HttpStatus::NOT_IMPLEMENTED, "Not Implemented"},
    {HttpStatus::BAD_GATEWAY, "Bad Gateway"},
    {HttpStatus::SERVICE_UNAVAILABLE, "Service Unavailable"},
    {HttpStatus::GATEWAY_TIMEOUT, "Gateway Timeout"},
    {HttpStatus::HTTP_VERSION_NOT_SUPPORTED, "HTTP Version Not Supported"},
    {HttpStatus::VARIANT_ALSO_NEGOTIATES, "Variant Also Negotiates"},
    {HttpStatus::INSUFFICIENT_STORAGE, "Insufficient Storage"},
    {HttpStatus::LOOP_DETECTED, "Loop Detected"},
    {HttpStatus::NOT_EXTENDED, "Not Extended"},
    {HttpStatus::NETWORK_AUTHENTICATION_REQUIRED,
     "Network Authentication Required"},
};

[[nodiscard]] constexpr unsigned char AsciiLower(unsigned char value) noexcept {
  return value >= 'A' && value <= 'Z'
             ? static_cast<unsigned char>(value + ('a' - 'A'))
             : value;
}

} // namespace

HttpMethod StringToHttpMethod(std::string_view method) noexcept {
  const auto iterator =
      std::find(kMethodNames.begin(), kMethodNames.end(), method);
  if (iterator == kMethodNames.end()) {
    return HttpMethod::INVALID_METHOD;
  }
  return static_cast<HttpMethod>(std::distance(kMethodNames.begin(), iterator));
}

HttpMethod CharsToHttpMethod(const char *method) noexcept {
  return method == nullptr ? HttpMethod::INVALID_METHOD
                           : StringToHttpMethod(method);
}

const char *HttpMethodToString(HttpMethod method) noexcept {
  const auto index = static_cast<std::size_t>(method);
  return index < kMethodNames.size() ? kMethodNames[index].data()
                                     : "INVALID_METHOD";
}

const char *HttpStatusToString(HttpStatus status) noexcept {
  const auto iterator = std::find_if(
      std::begin(kStatusNames), std::end(kStatusNames),
      [status](const auto &entry) { return entry.first == status; });
  return iterator == std::end(kStatusNames) ? "Unknown" : iterator->second;
}

bool CaseInsensitiveLess::operator()(std::string_view lhs,
                                     std::string_view rhs) const noexcept {
  return std::lexicographical_compare(
      lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
      [](char left, char right) {
        return AsciiLower(static_cast<unsigned char>(left)) <
               AsciiLower(static_cast<unsigned char>(right));
      });
}

} // namespace droplet::http
