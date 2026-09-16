#include <droplet/http/http_server.h>
#include <droplet/iomanager/iomanager.h>
#include <droplet/socket/address.h>
#include <droplet/types.h>

#include <pthread.h>
#include <signal.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

namespace {

bool ParsePort(std::string_view text, u16 &port) {
  unsigned int value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      value > 65535) {
    return false;
  }
  port = static_cast<u16>(value);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  u16 port = 8080;
  if (argc > 2 || (argc == 2 && !ParsePort(argv[1], port))) {
    std::println(stderr, "usage: {} [port]", argv[0]);
    return 1;
  }

  sigset_t signals;
  ::sigemptyset(&signals);
  ::sigaddset(&signals, SIGINT);
  ::sigaddset(&signals, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    std::println(stderr, "failed to block SIGINT/SIGTERM");
    return 1;
  }

  droplet::IOManager io_worker(2, false, "http-io");
  droplet::IOManager accept_worker(1, false, "http-accept");
  io_worker.start();
  accept_worker.start();

  auto server = std::make_shared<droplet::http::HttpServer>(
      true, &io_worker, &io_worker, &accept_worker);
  server->setName("droplet-http/0.1.0");
  const auto dispatch = server->getServletDispatch();

  dispatch->addServlet("/", [](auto request, auto response, auto session) {
    (void)request;
    (void)session;
    response->setHeader("Content-Type", "text/plain; charset=utf-8");
    response->setBody("droplet HTTP server\nTry /hello?name=your-name\n");
    return i32{0};
  });
  dispatch->addServlet("/hello", [](auto request, auto response, auto session) {
    (void)session;
    response->setHeader("Content-Type", "text/plain; charset=utf-8");
    response->setBody("hello, " + request->getParam("name", "world") + "!\n");
    return i32{0};
  });
  dispatch->addGlobServlet(
      "/api/*", [](auto request, auto response, auto session) {
        (void)session;
        response->setHeader("Content-Type", "text/plain; charset=utf-8");
        response->setBody("matched glob route: " + request->getPath() + "\n");
        return i32{0};
      });

  const auto address = droplet::IPv4Address::Create("0.0.0.0", port);
  if (!address || !server->bind(address) || !server->start()) {
    std::println(stderr, "failed to start HTTP server on port {}", port);
    server->stop();
    accept_worker.stop();
    io_worker.stop();
    return 1;
  }

  const auto listeners = server->getSockets();
  const auto local =
      listeners.empty() ? nullptr : listeners.front()->getLocalAddress();
  const auto local_ip = std::dynamic_pointer_cast<droplet::IPAddress>(local);
  const u32 actual_port = local_ip ? local_ip->getPort() : port;
  std::println("HTTP server listening on {}",
               local ? local->toString() : address->toString());
  std::println("Try: curl 'http://127.0.0.1:{}/hello?name=droplet'",
               actual_port);
  std::println("Press Ctrl-C to stop.");

  int signal = 0;
  (void)::sigwait(&signals, &signal);
  server->stop();
  accept_worker.stop();
  io_worker.stop();
  return 0;
}
