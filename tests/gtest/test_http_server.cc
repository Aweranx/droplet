#include <droplet/http/http_server.h>
#include <droplet/iomanager/iomanager.h>
#include <droplet/socket/address.h>
#include <droplet/types.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr auto kWait = std::chrono::seconds(2);

bool SendAll(int socket, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t count =
        ::send(socket, data.data() + offset, data.size() - offset, 0);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::string ReceiveUntilClosed(int socket) {
  std::string result;
  char buffer[4096];
  while (true) {
    const ssize_t count = ::recv(socket, buffer, sizeof(buffer), 0);
    if (count == 0) {
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    result.append(buffer, static_cast<std::size_t>(count));
  }
  return result;
}

std::size_t CountOccurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

TEST(HttpServerTest, DispatchesKeepAliveAndPipelinedRequests) {
  droplet::IOManager io_worker(2, false, "http-test-io");
  droplet::IOManager accept_worker(1, false, "http-test-accept");
  io_worker.start();
  accept_worker.start();

  auto server = std::make_shared<droplet::http::HttpServer>(
      true, &io_worker, &io_worker, &accept_worker);
  server->setName("droplet-http-test");
  server->setRecvTimeout(1000);

  std::promise<void> handled_promise;
  auto handled_future = handled_promise.get_future();
  std::atomic<int> handled{0};
  const auto mark_handled = [&] {
    if (handled.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
      handled_promise.set_value();
    }
  };

  const auto dispatch = server->getServletDispatch();
  ASSERT_NE(dispatch, nullptr);
  dispatch->addServlet(
      "/hello", [&](auto request, auto response, auto session) {
        (void)session;
        response->setHeader("Content-Type", "text/plain");
        response->setBody("hello " + request->getParam("name", "world"));
        mark_handled();
        return i32{0};
      });
  dispatch->addGlobServlet("/assets/*",
                           [&](auto request, auto response, auto session) {
                             (void)session;
                             response->setHeader("Content-Type", "text/plain");
                             response->setBody("asset " + request->getPath());
                             mark_handled();
                             return i32{0};
                           });

  const auto bind_address = droplet::IPv4Address::Create("127.0.0.1", 0);
  ASSERT_NE(bind_address, nullptr);
  ASSERT_TRUE(server->bind(bind_address));
  const auto local = std::dynamic_pointer_cast<droplet::IPv4Address>(
      server->getSockets().front()->getLocalAddress());
  ASSERT_NE(local, nullptr);
  ASSERT_TRUE(server->start());

  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client, 0);
  timeval timeout{};
  timeout.tv_sec = 2;
  ASSERT_EQ(
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
      0);
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons(static_cast<u16>(local->getPort()));
  ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&destination),
                      sizeof(destination)),
            0);

  const std::string requests =
      "GET /hello?name=droplet HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: keep-alive\r\n\r\n"
      "GET /assets/app.js HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: close\r\n\r\n";
  ASSERT_TRUE(SendAll(client, requests));
  const std::string responses = ReceiveUntilClosed(client);
  EXPECT_EQ(::close(client), 0);

  ASSERT_EQ(handled_future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(handled.load(std::memory_order_acquire), 2);
  EXPECT_EQ(CountOccurrences(responses, "HTTP/1.1 200 OK\r\n"), 2u);
  EXPECT_NE(responses.find("Server: droplet-http-test\r\n"), std::string::npos);
  EXPECT_NE(responses.find("hello droplet"), std::string::npos);
  EXPECT_NE(responses.find("asset /assets/app.js"), std::string::npos);
  EXPECT_NE(responses.find("Connection: keep-alive\r\n"), std::string::npos);
  EXPECT_NE(responses.find("Connection: close\r\n"), std::string::npos);

  server->stop();
  accept_worker.stop();
  io_worker.stop();
}

} // namespace
