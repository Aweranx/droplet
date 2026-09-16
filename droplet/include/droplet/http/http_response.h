#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/http/http.h>

#include <cstdint>
#include <ctime>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace droplet::http {

/** @brief 一个 HTTP 响应的数据模型。 */
class DROPLET_API HttpResponse {
public:
  using Ptr = std::shared_ptr<HttpResponse>;
  using MapType = HttpMap;

  explicit HttpResponse(u8 version = 0x11, bool close = true);

  [[nodiscard]] HttpStatus getStatus() const noexcept { return status_; }
  [[nodiscard]] u8 getVersion() const noexcept { return version_; }
  [[nodiscard]] const std::string &getBody() const noexcept { return body_; }
  [[nodiscard]] const std::string &getReason() const noexcept {
    return reason_;
  }
  [[nodiscard]] const MapType &getHeaders() const noexcept { return headers_; }
  [[nodiscard]] const std::vector<std::string> &getCookies() const noexcept {
    return cookies_;
  }
  [[nodiscard]] bool isClose() const noexcept { return close_; }
  [[nodiscard]] bool isWebsocket() const noexcept { return websocket_; }

  void setStatus(HttpStatus value) noexcept { status_ = value; }
  void setVersion(u8 value) noexcept { version_ = value; }
  void setBody(std::string value) { body_ = std::move(value); }
  void setReason(std::string value) { reason_ = std::move(value); }
  void setHeaders(MapType value) { headers_ = std::move(value); }
  void setClose(bool value) noexcept { close_ = value; }
  void setWebsocket(bool value) noexcept { websocket_ = value; }

  [[nodiscard]] std::string getHeader(std::string_view key,
                                      std::string default_value = {}) const;
  void setHeader(std::string key, std::string value);
  void delHeader(std::string_view key);
  [[nodiscard]] bool hasHeader(std::string_view key,
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

  void setRedirect(std::string uri);
  void setCookie(std::string key, std::string value, std::time_t expires = 0,
                 std::string path = {}, std::string domain = {},
                 bool secure = false, bool http_only = false,
                 std::string same_site = {});
  /** @brief 保存响应中已经格式化的单条 Set-Cookie 值。 */
  void addCookie(std::string value) { cookies_.push_back(std::move(value)); }

  /** @brief 根据 HTTP 版本和 Connection 头更新连接关闭语义。 */
  void initConnection();

  std::ostream &dump(std::ostream &stream) const;
  [[nodiscard]] std::string toString() const;

private:
  HttpStatus status_{HttpStatus::OK};
  u8 version_{};
  bool close_{};
  bool websocket_{false};
  std::string body_;
  std::string reason_;
  MapType headers_;
  std::vector<std::string> cookies_;
};

DROPLET_API std::ostream &operator<<(std::ostream &stream,
                                     const HttpResponse &response);

} // namespace droplet::http
