#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/http/http.h>

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace droplet::http {

class HttpResponse;

/** @brief 一个 HTTP 请求的数据模型。 */
class DROPLET_API HttpRequest {
public:
  using Ptr = std::shared_ptr<HttpRequest>;
  using MapType = HttpMap;

  explicit HttpRequest(u8 version = 0x11, bool close = true);

  [[nodiscard]] std::shared_ptr<HttpResponse> createResponse() const;

  [[nodiscard]] HttpMethod getMethod() const noexcept { return method_; }
  [[nodiscard]] u8 getVersion() const noexcept { return version_; }
  [[nodiscard]] const std::string &getPath() const noexcept { return path_; }
  [[nodiscard]] const std::string &getQuery() const noexcept { return query_; }
  [[nodiscard]] const std::string &getFragment() const noexcept {
    return fragment_;
  }
  [[nodiscard]] const std::string &getBody() const noexcept { return body_; }
  [[nodiscard]] const MapType &getHeaders() const noexcept { return headers_; }
  [[nodiscard]] const MapType &getParams() const;
  [[nodiscard]] const MapType &getCookies() const;

  void setMethod(HttpMethod value) noexcept { method_ = value; }
  void setVersion(u8 value) noexcept { version_ = value; }
  void setPath(std::string value) { path_ = std::move(value); }
  void setQuery(std::string value);
  void setFragment(std::string value) { fragment_ = std::move(value); }
  void setBody(std::string value);
  void setClose(bool value) noexcept { close_ = value; }
  void setWebsocket(bool value) noexcept { websocket_ = value; }
  void setHeaders(MapType value);
  void setParams(MapType value);
  void setCookies(MapType value);

  [[nodiscard]] bool isClose() const noexcept { return close_; }
  [[nodiscard]] bool isWebsocket() const noexcept { return websocket_; }

  [[nodiscard]] std::string getHeader(std::string_view key,
                                      std::string default_value = {}) const;
  [[nodiscard]] std::string getParam(std::string_view key,
                                     std::string default_value = {}) const;
  [[nodiscard]] std::string getCookie(std::string_view key,
                                      std::string default_value = {}) const;

  void setHeader(std::string key, std::string value);
  void setParam(std::string key, std::string value);
  void setCookie(std::string key, std::string value);
  void delHeader(std::string_view key);
  void delParam(std::string_view key);
  void delCookie(std::string_view key);

  [[nodiscard]] bool hasHeader(std::string_view key,
                               std::string *value = nullptr) const;
  [[nodiscard]] bool hasParam(std::string_view key,
                              std::string *value = nullptr) const;
  [[nodiscard]] bool hasCookie(std::string_view key,
                               std::string *value = nullptr) const;

  template <class T>
  bool checkGetHeaderAs(std::string_view key, T &value,
                        const T &default_value = T{}) const {
    return detail::CheckGetAs(headers_, key, value, default_value);
  }

  template <class T>
  [[nodiscard]] T getHeaderAs(std::string_view key,
                              const T &default_value = T{}) const {
    return detail::GetAs(headers_, key, default_value);
  }

  template <class T>
  bool checkGetParamAs(std::string_view key, T &value,
                       const T &default_value = T{}) const {
    initParams();
    return detail::CheckGetAs(params_, key, value, default_value);
  }

  template <class T>
  [[nodiscard]] T getParamAs(std::string_view key,
                             const T &default_value = T{}) const {
    initParams();
    return detail::GetAs(params_, key, default_value);
  }

  template <class T>
  bool checkGetCookieAs(std::string_view key, T &value,
                        const T &default_value = T{}) const {
    initCookies();
    return detail::CheckGetAs(cookies_, key, value, default_value);
  }

  template <class T>
  [[nodiscard]] T getCookieAs(std::string_view key,
                              const T &default_value = T{}) const {
    initCookies();
    return detail::GetAs(cookies_, key, default_value);
  }

  /** @brief 根据 HTTP 版本和 Connection 头更新连接关闭语义。 */
  void initConnection();

  std::ostream &dump(std::ostream &stream) const;
  [[nodiscard]] std::string toString() const;

private:
  void initParams() const;
  void initCookies() const;

  HttpMethod method_{HttpMethod::GET};
  u8 version_{};
  bool close_{};
  bool websocket_{false};
  std::string path_{"/"};
  std::string query_;
  std::string fragment_;
  std::string body_;
  MapType headers_;
  mutable MapType params_;
  mutable MapType cookies_;
  mutable bool params_initialized_{false};
  mutable bool cookies_initialized_{false};
};

DROPLET_API std::ostream &operator<<(std::ostream &stream,
                                     const HttpRequest &request);

} // namespace droplet::http
