#include <droplet/http/http_session.h>
#include <droplet/socket/address.h>
#include <droplet/socket/socket.h>
#include <droplet/stream/socket_stream.h>

#include <gtest/gtest.h>

#include <string>

namespace {

struct ConnectedSockets {
  droplet::Socket::Ptr listener;
  droplet::Socket::Ptr client;
  droplet::Socket::Ptr server;
};

ConnectedSockets MakeConnectedSockets() {
  ConnectedSockets result;
  result.listener = droplet::Socket::CreateTCPSocket();
  auto address = droplet::IPv4Address::Create("127.0.0.1", 0);
  if (!result.listener || !address || !result.listener->bind(address) ||
      !result.listener->listen()) {
    return result;
  }

  result.client = droplet::Socket::CreateTCPSocket();
  const auto local_address = result.listener->getLocalAddress();
  if (!result.client || !local_address ||
      !result.client->connect(local_address, 1000)) {
    return result;
  }
  result.server = result.listener->accept();
  return result;
}

TEST(HttpSessionTest, ReceivesPipelinedRequestsAndSendsAResponse) {
  auto sockets = MakeConnectedSockets();
  ASSERT_NE(sockets.client, nullptr);
  ASSERT_NE(sockets.server, nullptr);

  droplet::SocketStream client_stream(sockets.client, false);
  droplet::http::HttpSession session(sockets.server, false);
  const std::string requests =
      "GET /first?value=1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "POST /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
      "Content-Length: 5\r\n\r\nhello";
  ASSERT_EQ(client_stream.writeFixSize(requests.data(), requests.size()),
            static_cast<int>(requests.size()));

  const auto first = session.recvRequest();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->getPath(), "/first");
  EXPECT_EQ(first->getParamAs<int>("value"), 1);
  EXPECT_FALSE(first->isClose());

  const auto second = session.recvRequest();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->getMethod(), droplet::http::HttpMethod::POST);
  EXPECT_EQ(second->getPath(), "/second");
  EXPECT_EQ(second->getBody(), "hello");
  EXPECT_TRUE(second->isClose());

  auto response = second->createResponse();
  response->setStatus(droplet::http::HttpStatus::CREATED);
  response->setBody("world");
  const std::string expected = response->toString();
  ASSERT_EQ(session.sendResponse(response), static_cast<int>(expected.size()));

  std::string received(expected.size(), '\0');
  ASSERT_EQ(client_stream.readFixSize(received.data(), received.size()),
            static_cast<int>(received.size()));
  EXPECT_EQ(received, expected);
}

TEST(HttpSessionTest, ClosesConnectionOnMalformedRequest) {
  auto sockets = MakeConnectedSockets();
  ASSERT_NE(sockets.client, nullptr);
  ASSERT_NE(sockets.server, nullptr);

  droplet::SocketStream client_stream(sockets.client, false);
  droplet::http::HttpSession session(sockets.server, false);
  const std::string malformed =
      "BREW /coffee HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_EQ(client_stream.writeFixSize(malformed.data(), malformed.size()),
            static_cast<int>(malformed.size()));

  EXPECT_EQ(session.recvRequest(), nullptr);
  EXPECT_EQ(session.getLastParseError(),
            droplet::http::HttpParseError::INVALID_METHOD);
  EXPECT_FALSE(session.getLastParseErrorMessage().empty());
  EXPECT_FALSE(session.isConnected());
}

} // namespace
