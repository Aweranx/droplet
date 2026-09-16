#include <droplet/http/http_server.h>

#include <droplet/http/http_session.h>
#include <droplet/logger/log.h>

#include <exception>
#include <mutex>
#include <utility>

namespace droplet::http {
namespace {

LoggerPtr HttpServerLogger() {
  static auto logger = GetLogger("system");
  return logger;
}

} // namespace

HttpServer::HttpServer(bool keep_alive, IOManager *worker, IOManager *io_worker,
                       IOManager *accept_worker)
    : TcpServer(worker, io_worker, accept_worker), keep_alive_(keep_alive),
      dispatch_(std::make_shared<ServletDispatch>(getName())) {
  setType("http");
}

ServletDispatch::Ptr HttpServer::getServletDispatch() const {
  std::shared_lock lock(dispatch_mutex_);
  return dispatch_;
}

void HttpServer::setServletDispatch(ServletDispatch::Ptr dispatch) {
  std::unique_lock lock(dispatch_mutex_);
  dispatch_ = dispatch ? std::move(dispatch)
                       : std::make_shared<ServletDispatch>(getName());
}

void HttpServer::setName(std::string name) {
  TcpServer::setName(name);
  const auto dispatch = getServletDispatch();
  if (dispatch) {
    dispatch->setDefault(std::make_shared<NotFoundServlet>(std::move(name)));
  }
}

void HttpServer::handleClient(Socket::Ptr client) {
  auto session = std::make_shared<HttpSession>(std::move(client));
  while (session->isConnected()) {
    auto request = session->recvRequest();
    if (!request) {
      break;
    }

    auto response = request->createResponse();
    response->setHeader("Server", getName());
    const bool server_allows_keep_alive = isKeepAlive();
    response->setClose(request->isClose() || !server_allows_keep_alive);

    try {
      const auto dispatch = getServletDispatch();
      if (dispatch) {
        (void)dispatch->handle(request, response, session);
      } else {
        response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
        response->setBody("No servlet dispatcher is configured");
      }
    } catch (const std::exception &exception) {
      DROPLET_LOG_ERROR(HttpServerLogger())
          << "HTTP servlet threw an exception: " << exception.what();
      response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
      response->setBody("Internal Server Error");
    } catch (...) {
      DROPLET_LOG_ERROR(HttpServerLogger())
          << "HTTP servlet threw an unknown exception";
      response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
      response->setBody("Internal Server Error");
    }

    const bool close_after_response =
        response->isClose() || request->isClose() || !server_allows_keep_alive;
    response->setClose(close_after_response);
    // HEAD 只发送与 GET 对应的响应头，不能把正文留在持久连接中污染下一条响应。
    if (request->getMethod() == HttpMethod::HEAD) {
      response->setBody({});
    }
    if (session->sendResponse(response) <= 0 || close_after_response) {
      break;
    }
  }
  session->close();
}

} // namespace droplet::http
