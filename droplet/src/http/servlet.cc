#include <droplet/http/servlet.h>
#include <droplet/types.h>

#include <algorithm>
#include <mutex>

#include <fnmatch.h>

namespace droplet::http {

FunctionServlet::FunctionServlet(Callback callback)
    : Servlet("FunctionServlet"), callback_(std::move(callback)) {}

i32 FunctionServlet::handle(HttpRequest::Ptr request,
                            HttpResponse::Ptr response,
                            HttpSession::Ptr session) {
  if (callback_) {
    return callback_(std::move(request), std::move(response),
                     std::move(session));
  }
  if (response) {
    response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
    response->setBody("Servlet callback is empty");
  }
  return -1;
}

ServletDispatch::ServletDispatch(std::string server_name)
    : Servlet("ServletDispatch"),
      default_(std::make_shared<NotFoundServlet>(std::move(server_name))) {}

i32 ServletDispatch::handle(HttpRequest::Ptr request,
                            HttpResponse::Ptr response,
                            HttpSession::Ptr session) {
  if (!request || !response) {
    return -1;
  }
  auto servlet = getMatchedServlet(request->getPath());
  if (!servlet) {
    response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
    response->setBody("No default servlet is configured");
    return -1;
  }
  return servlet->handle(std::move(request), std::move(response),
                         std::move(session));
}

void ServletDispatch::addServlet(std::string uri, Servlet::Ptr servlet) {
  if (!servlet) {
    (void)delServlet(uri);
    return;
  }
  std::unique_lock lock(mutex_);
  exact_routes_.insert_or_assign(std::move(uri), std::move(servlet));
}

void ServletDispatch::addServlet(std::string uri,
                                 FunctionServlet::Callback callback) {
  addServlet(std::move(uri),
             std::make_shared<FunctionServlet>(std::move(callback)));
}

void ServletDispatch::addGlobServlet(std::string pattern,
                                     Servlet::Ptr servlet) {
  if (!servlet) {
    (void)delGlobServlet(pattern);
    return;
  }

  std::unique_lock lock(mutex_);
  const auto existing = std::find_if(
      glob_routes_.begin(), glob_routes_.end(),
      [&pattern](const auto &route) { return route.first == pattern; });
  if (existing != glob_routes_.end()) {
    glob_routes_.erase(existing);
  }
  glob_routes_.emplace_back(std::move(pattern), std::move(servlet));
}

void ServletDispatch::addGlobServlet(std::string pattern,
                                     FunctionServlet::Callback callback) {
  addGlobServlet(std::move(pattern),
                 std::make_shared<FunctionServlet>(std::move(callback)));
}

bool ServletDispatch::delServlet(std::string_view uri) {
  std::unique_lock lock(mutex_);
  const auto iterator = exact_routes_.find(std::string(uri));
  if (iterator == exact_routes_.end()) {
    return false;
  }
  exact_routes_.erase(iterator);
  return true;
}

bool ServletDispatch::delGlobServlet(std::string_view pattern) {
  std::unique_lock lock(mutex_);
  const auto iterator = std::find_if(
      glob_routes_.begin(), glob_routes_.end(),
      [pattern](const auto &route) { return route.first == pattern; });
  if (iterator == glob_routes_.end()) {
    return false;
  }
  glob_routes_.erase(iterator);
  return true;
}

Servlet::Ptr ServletDispatch::getServlet(std::string_view uri) const {
  std::shared_lock lock(mutex_);
  const auto iterator = exact_routes_.find(std::string(uri));
  return iterator == exact_routes_.end() ? nullptr : iterator->second;
}

Servlet::Ptr ServletDispatch::getGlobServlet(std::string_view pattern) const {
  std::shared_lock lock(mutex_);
  const auto iterator = std::find_if(
      glob_routes_.begin(), glob_routes_.end(),
      [pattern](const auto &route) { return route.first == pattern; });
  return iterator == glob_routes_.end() ? nullptr : iterator->second;
}

Servlet::Ptr ServletDispatch::getMatchedServlet(std::string_view uri) const {
  const std::string uri_string(uri);
  std::shared_lock lock(mutex_);
  const auto exact = exact_routes_.find(uri_string);
  if (exact != exact_routes_.end()) {
    return exact->second;
  }
  for (const auto &[pattern, servlet] : glob_routes_) {
    if (::fnmatch(pattern.c_str(), uri_string.c_str(), 0) == 0) {
      return servlet;
    }
  }
  return default_;
}

Servlet::Ptr ServletDispatch::getDefault() const {
  std::shared_lock lock(mutex_);
  return default_;
}

void ServletDispatch::setDefault(Servlet::Ptr servlet) {
  std::unique_lock lock(mutex_);
  default_ = std::move(servlet);
}

NotFoundServlet::NotFoundServlet(std::string server_name)
    : Servlet("NotFoundServlet"), server_name_(std::move(server_name)) {
  content_ = "<!doctype html><html><head><title>404 Not Found</title></head>"
             "<body><h1>404 Not Found</h1><hr><p>" +
             server_name_ + "</p></body></html>";
}

i32 NotFoundServlet::handle(HttpRequest::Ptr request,
                            HttpResponse::Ptr response,
                            HttpSession::Ptr session) {
  (void)request;
  (void)session;
  if (!response) {
    return -1;
  }
  response->setStatus(HttpStatus::NOT_FOUND);
  if (!response->hasHeader("Server")) {
    response->setHeader("Server", server_name_);
  }
  response->setHeader("Content-Type", "text/html; charset=utf-8");
  response->setBody(content_);
  return 0;
}

} // namespace droplet::http
