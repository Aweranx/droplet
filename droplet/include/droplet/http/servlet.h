#pragma once

#include <droplet/types.h>

#include <droplet/export.h>
#include <droplet/http/http_request.h>
#include <droplet/http/http_response.h>
#include <droplet/http/http_session.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace droplet::http {

/** @brief HTTP 请求处理器的统一接口。 */
class DROPLET_API Servlet {
public:
  using Ptr = std::shared_ptr<Servlet>;

  explicit Servlet(std::string name) : name_(std::move(name)) {}
  virtual ~Servlet() = default;

  virtual i32 handle(HttpRequest::Ptr request, HttpResponse::Ptr response,
                     HttpSession::Ptr session) = 0;

  [[nodiscard]] const std::string &getName() const noexcept { return name_; }

private:
  std::string name_;
};

/** @brief 把 Lambda、普通函数等可调用对象包装成 Servlet。 */
class DROPLET_API FunctionServlet final : public Servlet {
public:
  using Ptr = std::shared_ptr<FunctionServlet>;
  using Callback =
      std::function<i32(HttpRequest::Ptr, HttpResponse::Ptr, HttpSession::Ptr)>;

  explicit FunctionServlet(Callback callback);

  i32 handle(HttpRequest::Ptr request, HttpResponse::Ptr response,
             HttpSession::Ptr session) override;

private:
  Callback callback_;
};

/**
 * @brief 线程安全的 Servlet 路由表。
 *
 * 查找顺序为精确路由、按注册顺序排列的 glob 路由、默认 Servlet。
 */
class DROPLET_API ServletDispatch final : public Servlet {
public:
  using Ptr = std::shared_ptr<ServletDispatch>;

  explicit ServletDispatch(std::string server_name = "droplet/0.1.0");

  i32 handle(HttpRequest::Ptr request, HttpResponse::Ptr response,
             HttpSession::Ptr session) override;

  void addServlet(std::string uri, Servlet::Ptr servlet);
  void addServlet(std::string uri, FunctionServlet::Callback callback);
  void addGlobServlet(std::string pattern, Servlet::Ptr servlet);
  void addGlobServlet(std::string pattern, FunctionServlet::Callback callback);

  bool delServlet(std::string_view uri);
  bool delGlobServlet(std::string_view pattern);

  [[nodiscard]] Servlet::Ptr getServlet(std::string_view uri) const;
  /** @brief 按注册时的 pattern 精确查找，不执行通配符匹配。 */
  [[nodiscard]] Servlet::Ptr getGlobServlet(std::string_view pattern) const;
  [[nodiscard]] Servlet::Ptr getMatchedServlet(std::string_view uri) const;

  [[nodiscard]] Servlet::Ptr getDefault() const;
  void setDefault(Servlet::Ptr servlet);

private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, Servlet::Ptr> exact_routes_;
  std::vector<std::pair<std::string, Servlet::Ptr>> glob_routes_;
  Servlet::Ptr default_;
};

/** @brief 没有任何路由匹配时生成 404 响应。 */
class DROPLET_API NotFoundServlet final : public Servlet {
public:
  using Ptr = std::shared_ptr<NotFoundServlet>;

  explicit NotFoundServlet(std::string server_name = "droplet/0.1.0");

  i32 handle(HttpRequest::Ptr request, HttpResponse::Ptr response,
             HttpSession::Ptr session) override;

private:
  std::string server_name_;
  std::string content_;
};

} // namespace droplet::http
