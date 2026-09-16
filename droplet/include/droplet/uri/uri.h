#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/socket/address.h>

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace droplet {

/** @brief RFC 3986 URI 的轻量数据模型与解析器。 */
class DROPLET_API Uri {
public:
  using Ptr = std::shared_ptr<Uri>;

  /** @return 解析成功的 URI；语法非法时返回 nullptr。 */
  [[nodiscard]] static Ptr Create(std::string_view text);

  Uri() = default;

  [[nodiscard]] const std::string &getScheme() const noexcept {
    return scheme_;
  }
  [[nodiscard]] const std::string &getUserinfo() const noexcept {
    return userinfo_;
  }
  [[nodiscard]] const std::string &getHost() const noexcept { return host_; }
  [[nodiscard]] const std::string &getPath() const noexcept;
  [[nodiscard]] const std::string &getQuery() const noexcept { return query_; }
  [[nodiscard]] const std::string &getFragment() const noexcept {
    return fragment_;
  }
  [[nodiscard]] i32 getPort() const noexcept;
  [[nodiscard]] bool hasAuthority() const noexcept { return has_authority_; }
  [[nodiscard]] bool isDefaultPort() const noexcept;

  void setScheme(std::string value);
  void setUserinfo(std::string value) { userinfo_ = std::move(value); }
  void setHost(std::string value) {
    host_ = std::move(value);
    has_authority_ = true;
  }
  void setPath(std::string value) { path_ = std::move(value); }
  void setQuery(std::string value) { query_ = std::move(value); }
  void setFragment(std::string value) { fragment_ = std::move(value); }
  void setPort(i32 value) noexcept { port_ = value; }

  std::ostream &dump(std::ostream &stream) const;
  [[nodiscard]] std::string toString() const;

  /** @brief 解析主机并把 URI 端口写入返回的 IPAddress。 */
  [[nodiscard]] IPAddress::Ptr createAddress() const;

private:
  std::string scheme_;
  std::string userinfo_;
  std::string host_;
  std::string path_;
  std::string query_;
  std::string fragment_;
  i32 port_{-1};
  bool has_authority_{false};
};

DROPLET_API std::ostream &operator<<(std::ostream &stream, const Uri &uri);

} // namespace droplet
