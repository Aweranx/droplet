#include <droplet/socket/address.h>
#include <droplet/types.h>
#include <droplet/hook/hook.h>
#include <droplet/iomanager/iomanager.h>
#include <droplet/socket/socket.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using droplet::Fiber;
using droplet::IOManager;
using droplet::Socket;

constexpr auto kWait = std::chrono::seconds(2);

TEST(TestSocket, TcpSocketRoundTripThroughIOManager) {
  IOManager iom(2, false, "socket-tcp-round-trip");
  iom.start();

  std::promise<u16> port_promise;
  auto port_future = port_promise.get_future();
  std::promise<bool> server_done;
  auto server_future = server_done.get_future();

  iom.schedule([&] {
    bool ok = false;
    bool published = false;
    auto server = Socket::CreateTCPSocket();
    auto bind_address =
        std::make_shared<droplet::IPv4Address>(INADDR_LOOPBACK, 0);
    if (server && server->bind(bind_address) && server->listen()) {
      auto local = std::dynamic_pointer_cast<droplet::IPv4Address>(
          server->getLocalAddress());
      if (local) {
        port_promise.set_value(static_cast<u16>(local->getPort()));
        published = true;
        auto client = server->accept();
        if (client) {
          char request[4]{};
          const ssize_t received = client->recv(request, sizeof(request));
          if (received == 4 && std::string(request, sizeof(request)) == "ping") {
            constexpr char response[] = "pong";
            ok = client->send(response, sizeof(response) - 1) ==
                 static_cast<ssize_t>(sizeof(response) - 1);
          }
        }
      }
    }
    if (!published) {
      // The normal path publishes the port before accept; retain a value for
      // failure paths so the test thread never waits forever.
      port_promise.set_value(0);
    }
    server_done.set_value(ok);
  });

  ASSERT_EQ(port_future.wait_for(kWait), std::future_status::ready);
  const u16 port = port_future.get();
  ASSERT_NE(port, 0);

  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client, 0);
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons(port);
  ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&destination),
                      sizeof(destination)),
            0);

  constexpr char request[] = "ping";
  ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0),
            static_cast<ssize_t>(sizeof(request) - 1));
  char response[4]{};
  ASSERT_EQ(::recv(client, response, sizeof(response), 0),
            static_cast<ssize_t>(sizeof(response)));
  EXPECT_EQ(std::string(response, sizeof(response)), "pong");
  EXPECT_EQ(server_future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(server_future.get());
  EXPECT_EQ(::close(client), 0);

  iom.stop();
}

TEST(TestSocket, UdpSendToAndReceiveFrom) {
  auto receiver = Socket::CreateUDPSocket();
  auto sender = Socket::CreateUDPSocket();
  ASSERT_NE(receiver, nullptr);
  ASSERT_NE(sender, nullptr);
  EXPECT_EQ(sender->getRecvTimeout(), -1);
  sender->setRecvTimeout(20);
  EXPECT_EQ(sender->getRecvTimeout(), 20);

  auto receiver_bind =
      std::make_shared<droplet::IPv4Address>(INADDR_LOOPBACK, 0);
  ASSERT_TRUE(receiver->bind(receiver_bind));
  auto receiver_address = std::dynamic_pointer_cast<droplet::IPv4Address>(
      receiver->getLocalAddress());
  ASSERT_NE(receiver_address, nullptr);
  ASSERT_NE(receiver_address->getPort(), 0u);

  constexpr char message[] = "datagram";
  ASSERT_EQ(sender->sendTo(message, sizeof(message) - 1, receiver_address),
            static_cast<ssize_t>(sizeof(message) - 1));

  auto from = std::make_shared<droplet::IPv4Address>();
  char received[sizeof(message)]{};
  ASSERT_EQ(receiver->recvFrom(received, sizeof(received) - 1, from),
            static_cast<ssize_t>(sizeof(message) - 1));
  EXPECT_EQ(std::string(received, sizeof(message) - 1), message);
  EXPECT_EQ(from->getFamily(), AF_INET);
}

TEST(TestSocket, ConnectUsesFiberHookAndTimeoutPath) {
  IOManager iom(1, false, "socket-connect-hook");
  iom.start();

  std::promise<u16> port_promise;
  auto port_future = port_promise.get_future();
  std::promise<bool> server_promise;
  auto server_future = server_promise.get_future();
  std::promise<bool> client_promise;
  auto client_future = client_promise.get_future();

  iom.schedule([&] {
    bool ok = false;
    auto server = Socket::CreateTCPSocket();
    auto bind_address =
        std::make_shared<droplet::IPv4Address>(INADDR_LOOPBACK, 0);
    if (server && server->bind(bind_address) && server->listen()) {
      auto local = std::dynamic_pointer_cast<droplet::IPv4Address>(
          server->getLocalAddress());
      if (local) {
        port_promise.set_value(static_cast<u16>(local->getPort()));
        auto client = server->accept();
        if (client) {
          char value[2]{};
          ok = client->recv(value, sizeof(value)) == 2 &&
               std::string(value, sizeof(value)) == "ok";
        }
      }
    }
    server_promise.set_value(ok);
  });

  ASSERT_EQ(port_future.wait_for(kWait), std::future_status::ready);
  const u16 port = port_future.get();
  ASSERT_NE(port, 0);
  iom.schedule([&, port] {
    auto client = Socket::CreateTCPSocket();
    auto destination =
        std::make_shared<droplet::IPv4Address>(INADDR_LOOPBACK, port);
    bool ok = client && client->connect(destination, 1000);
    if (ok) {
      constexpr char value[] = "ok";
      ok = client->send(value, sizeof(value) - 1) ==
           static_cast<ssize_t>(sizeof(value) - 1);
    }
    client_promise.set_value(ok);
  });

  EXPECT_EQ(client_future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(client_future.get());
  EXPECT_EQ(server_future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(server_future.get());
  iom.stop();
}

TEST(TestHook, EagainReadSuspendsAndResumesFiber) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  IOManager iom(1, false, "hook-eagain-read");
  iom.start();

  std::promise<int> peer_promise;
  auto peer_future = peer_promise.get_future();
  std::promise<bool> done_promise;
  auto done_future = done_promise.get_future();
  std::atomic<int> stage{0};

  iom.schedule([&] {
    bool ok = true;
    peer_promise.set_value(sockets[1]);
    stage.store(1, std::memory_order_release);
    char value = 0;
    const ssize_t count = ::recv(sockets[0], &value, sizeof(value), 0);
    ok = count == 1 && value == 'x';
    (void)::close(sockets[0]);
    (void)::close(sockets[1]);
    done_promise.set_value(ok);
  });

  ASSERT_EQ(peer_future.wait_for(kWait), std::future_status::ready);
  const int peer = peer_future.get();
  ASSERT_GE(peer, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(stage.load(std::memory_order_acquire), 1);

  const char value = 'x';
  ASSERT_EQ(::send(peer, &value, sizeof(value), 0), 1);
  ASSERT_EQ(done_future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(done_future.get());
  iom.stop();
}

TEST(TestHook, ReceiveTimeoutWakesFiberWithEtimedout) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  timeval timeout{};
  timeout.tv_usec = 20 * 1000;
  ASSERT_EQ(::setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)),
            0);
  IOManager iom(1, false, "hook-recv-timeout");
  iom.start();

  std::promise<int> error_promise;
  auto error_future = error_promise.get_future();
  iom.schedule([&] {
    int error = EIO;
    char value = 0;
    if (::recv(sockets[0], &value, sizeof(value), 0) == -1) {
      error = errno;
    }
    (void)::close(sockets[0]);
    (void)::close(sockets[1]);
    error_promise.set_value(error);
  });

  ASSERT_EQ(error_future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(error_future.get(), ETIMEDOUT);
  iom.stop();
}

TEST(TestHook, CloseCancelsPendingRead) {
  IOManager iom(2, false, "hook-close-cancel");
  iom.start();

  std::promise<std::pair<int, int>> sockets_promise;
  auto sockets_future = sockets_promise.get_future();
  std::promise<int> error_promise;
  auto error_future = error_promise.get_future();
  std::atomic<int> stage{0};

  iom.schedule([&] {
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
      sockets_promise.set_value({-1, -1});
      error_promise.set_value(errno);
      return;
    }
    sockets_promise.set_value({sockets[0], sockets[1]});
    stage.store(1, std::memory_order_release);
    char value = 0;
    const ssize_t result = ::recv(sockets[0], &value, sizeof(value), 0);
    const int error = result < 0 ? errno : 0;
    // The other worker closes sockets[0] and wakes this Fiber.  Do not close
    // it again here because the descriptor number could already be reused.
    error_promise.set_value(error);
  });

  ASSERT_EQ(sockets_future.wait_for(kWait), std::future_status::ready);
  const auto sockets = sockets_future.get();
  ASSERT_GE(sockets.first, 0);
  for (int i = 0; i < 200 && iom.pendingEventCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(stage.load(std::memory_order_acquire), 1);
  ASSERT_EQ(iom.pendingEventCount(), 1u);

  iom.schedule([fd = sockets.first] { (void)::close(fd); });
  ASSERT_EQ(error_future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(error_future.get(), EBADF);
  (void)::close(sockets.second);
  iom.stop();
}

TEST(TestHook, SleepAllowsAnotherFiberToRun) {
  IOManager iom(1, false, "hook-sleep");
  iom.start();

  std::promise<void> other_promise;
  auto other_future = other_promise.get_future();
  std::promise<void> sleeper_promise;
  auto sleeper_future = sleeper_promise.get_future();

  iom.schedule([&] {
    ::usleep(100 * 1000);
    sleeper_promise.set_value();
  });
  iom.schedule([&] { other_promise.set_value(); });

  EXPECT_EQ(other_future.wait_for(std::chrono::milliseconds(50)),
            std::future_status::ready);
  EXPECT_EQ(sleeper_future.wait_for(kWait), std::future_status::ready);
  iom.stop();
}

TEST(TestHook, KernelNonblockIsHiddenUntilUserRequestsIt) {
  IOManager iom(1, false, "hook-fcntl");
  iom.start();

  std::promise<bool> result_promise;
  auto result_future = result_promise.get_future();
  iom.schedule([&] {
    int sockets[2] = {-1, -1};
    bool ok = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0;
    if (ok) {
      const int initial = ::fcntl(sockets[0], F_GETFL);
      ok = initial >= 0 && (initial & O_NONBLOCK) == 0;
      ok = ok && ::fcntl(sockets[0], F_SETFL, initial | O_NONBLOCK) == 0;
      const int explicit_nonblock = ::fcntl(sockets[0], F_GETFL);
      ok = ok && (explicit_nonblock & O_NONBLOCK) != 0;
      (void)::close(sockets[0]);
      (void)::close(sockets[1]);
    }
    result_promise.set_value(ok);
  });

  ASSERT_EQ(result_future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(result_future.get());
  iom.stop();
}

}  // namespace
