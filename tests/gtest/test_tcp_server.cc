#include <droplet/iomanager/iomanager.h>
#include <droplet/types.h>
#include <droplet/socket/address.h>
#include <droplet/stream/socket_stream.h>
#include <droplet/tcpserver/tcp_server.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr auto kWait = std::chrono::seconds(2);

class EchoTcpServer final : public droplet::TcpServer {
 public:
  using TcpServer::TcpServer;

  std::future<bool> getHandledFuture() { return handled_.get_future(); }

 protected:
  void handleClient(droplet::Socket::Ptr client) override {
    bool ok =
        droplet::IOManager::GetThis() == getIoWorker() && client &&
        client->getRecvTimeout() == static_cast<i64>(getRecvTimeout());
    droplet::SocketStream stream(std::move(client));
    std::array<char, 256> buffer{};
    while (true) {
      const int count = stream.read(buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0 ||
          stream.writeFixSize(buffer.data(), static_cast<std::size_t>(count)) !=
              count) {
        ok = false;
        break;
      }
    }
    handled_.set_value(ok);
  }

 private:
  std::promise<bool> handled_;
};

TEST(TcpServerTest, EchoesDataAndStopsAcceptLoop) {
  droplet::IOManager io_worker(2, false, "tcp-test-io");
  droplet::IOManager accept_worker(1, false, "tcp-test-accept");
  io_worker.start();
  accept_worker.start();

  auto server =
      std::make_shared<EchoTcpServer>(&io_worker, &io_worker, &accept_worker);
  auto handled = server->getHandledFuture();
  server->setName("echo-test");
  server->setRecvTimeout(1000);

  auto bind_address = droplet::IPv4Address::Create("127.0.0.1", 0);
  ASSERT_NE(bind_address, nullptr);
  ASSERT_TRUE(server->bind(bind_address));
  ASSERT_EQ(server->getSockets().size(), 1u);
  auto server_address = server->getSockets().front()->getLocalAddress();
  ASSERT_NE(server_address, nullptr);

  ASSERT_TRUE(server->start());
  EXPECT_TRUE(server->start());
  EXPECT_FALSE(server->isStop());

  auto client = droplet::Socket::CreateTCP(server_address);
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->connect(server_address, 1000));
  client->setRecvTimeout(1000);

  droplet::SocketStream stream(client);
  const std::string message = "hello from TcpServer test";
  ASSERT_EQ(stream.writeFixSize(message.data(), message.size()),
            static_cast<int>(message.size()));
  std::string response(message.size(), '\0');
  ASSERT_EQ(stream.readFixSize(response.data(), response.size()),
            static_cast<int>(response.size()));
  EXPECT_EQ(response, message);
  stream.close();

  ASSERT_EQ(handled.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(handled.get());

  server->stop();
  server->stop();
  accept_worker.stop();
  io_worker.stop();
  EXPECT_TRUE(server->isStop());
  EXPECT_TRUE(server->getSockets().empty());
}

TEST(TcpServerTest, ReportsInvalidBindAndRequiresAListener) {
  droplet::IOManager iom(1, false, "tcp-test-invalid");
  auto server = std::make_shared<droplet::TcpServer>(&iom, &iom, &iom);

  std::vector<droplet::Address::Ptr> addresses{nullptr};
  std::vector<droplet::Address::Ptr> failures;
  EXPECT_FALSE(server->bind(addresses, failures));
  ASSERT_EQ(failures.size(), 1u);
  EXPECT_EQ(failures.front(), nullptr);

  errno = 0;
  EXPECT_FALSE(server->start());
  EXPECT_EQ(errno, EINVAL);
}

TEST(TcpServerTest, StopBeforeStartClosesBoundListeners) {
  droplet::IOManager iom(1, false, "tcp-test-stop-before-start");
  auto server = std::make_shared<droplet::TcpServer>(&iom, &iom, &iom);
  server->setName("metadata-test");
  server->setRecvTimeout(3210);

  auto address = droplet::IPv4Address::Create("127.0.0.1", 0);
  ASSERT_NE(address, nullptr);
  ASSERT_TRUE(server->bind(address));
  auto listener = server->getSockets().front();
  ASSERT_TRUE(listener->isValid());

  const std::string description = server->toString();
  EXPECT_NE(description.find("name=metadata-test"), std::string::npos);
  EXPECT_NE(description.find("recv_timeout=3210"), std::string::npos);

  server->stop();
  EXPECT_TRUE(server->isStop());
  EXPECT_TRUE(server->getSockets().empty());
  EXPECT_FALSE(listener->isValid());
}

TEST(TcpServerTest, MultiBindIsAllOrNothing) {
  droplet::IOManager iom(1, false, "tcp-test-multi-bind");

  auto occupied = droplet::Socket::CreateTCPSocket();
  auto occupied_address = droplet::IPv4Address::Create("127.0.0.1", 0);
  ASSERT_NE(occupied, nullptr);
  ASSERT_NE(occupied_address, nullptr);
  ASSERT_TRUE(occupied->bind(occupied_address));
  ASSERT_TRUE(occupied->listen());
  occupied_address = std::dynamic_pointer_cast<droplet::IPv4Address>(
      occupied->getLocalAddress());
  ASSERT_NE(occupied_address, nullptr);

  auto server = std::make_shared<droplet::TcpServer>(&iom, &iom, &iom);
  auto available_address = droplet::IPv4Address::Create("127.0.0.1", 0);
  std::vector<droplet::Address::Ptr> addresses{available_address,
                                               occupied_address};
  std::vector<droplet::Address::Ptr> failures;
  EXPECT_FALSE(server->bind(addresses, failures));
  ASSERT_EQ(failures.size(), 1u);
  EXPECT_EQ(failures.front(), occupied_address);
  EXPECT_TRUE(server->getSockets().empty());
}

}  // namespace
