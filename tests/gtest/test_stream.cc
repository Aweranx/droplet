#include <droplet/socket/address.h>
#include <droplet/socket/socket.h>
#include <droplet/stream/socket_stream.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
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
  if (!result.listener) {
    return result;
  }

  auto address = droplet::IPv4Address::Create("127.0.0.1", 0);
  if (!address || !result.listener->bind(address) ||
      !result.listener->listen()) {
    return result;
  }

  result.client = droplet::Socket::CreateTCPSocket();
  if (!result.client || !result.client->connect(result.listener->getLocalAddress())) {
    return result;
  }
  result.server = result.listener->accept();
  return result;
}

TEST(SocketStreamTest, RawBufferRoundTripAndAddresses) {
  auto sockets = MakeConnectedSockets();
  ASSERT_NE(sockets.listener, nullptr);
  ASSERT_NE(sockets.client, nullptr);
  ASSERT_NE(sockets.server, nullptr);

  droplet::SocketStream client_stream(sockets.client);
  droplet::SocketStream server_stream(sockets.server);
  ASSERT_TRUE(client_stream.isConnected());
  ASSERT_TRUE(server_stream.isConnected());
  EXPECT_FALSE(client_stream.getRemoteAddressString().empty());
  EXPECT_FALSE(server_stream.getLocalAddressString().empty());

  const std::string message = "socket stream raw message";
  ASSERT_EQ(client_stream.writeFixSize(message.data(), message.size()),
            static_cast<int>(message.size()));

  std::string received(message.size(), '\0');
  ASSERT_EQ(server_stream.readFixSize(received.data(), received.size()),
            static_cast<int>(message.size()));
  EXPECT_EQ(received, message);
}

TEST(SocketStreamTest, ByteArrayUsesScatterGatherBuffers) {
  auto sockets = MakeConnectedSockets();
  ASSERT_NE(sockets.listener, nullptr);
  ASSERT_NE(sockets.client, nullptr);
  ASSERT_NE(sockets.server, nullptr);

  droplet::SocketStream client_stream(sockets.client);
  droplet::SocketStream server_stream(sockets.server);

  auto outgoing = std::make_shared<droplet::ByteArray>(3);
  const std::string message = "a payload that crosses several byte array nodes";
  outgoing->writeStringWithoutLength(message);
  outgoing->setPosition(0);
  ASSERT_EQ(client_stream.writeFixSize(outgoing, message.size()),
            static_cast<int>(message.size()));
  EXPECT_EQ(outgoing->getReadSize(), 0u);

  auto incoming = std::make_shared<droplet::ByteArray>(2);
  ASSERT_EQ(server_stream.readFixSize(incoming, message.size()),
            static_cast<int>(message.size()));
  EXPECT_EQ(incoming->getPosition(), message.size());
  incoming->setPosition(0);
  EXPECT_EQ(incoming->toString(), message);
}

TEST(SocketStreamTest, NonOwnerDoesNotCloseSocketOnDestruction) {
  auto sockets = MakeConnectedSockets();
  ASSERT_NE(sockets.listener, nullptr);
  ASSERT_NE(sockets.client, nullptr);
  ASSERT_NE(sockets.server, nullptr);

  {
    droplet::SocketStream stream(sockets.client, false);
    ASSERT_TRUE(stream.isConnected());
  }
  EXPECT_TRUE(sockets.client->isValid());
  EXPECT_TRUE(sockets.client->isConnected());
}

TEST(StreamTest, FixedSizeReadAndWriteRejectNullByteArray) {
  auto sockets = MakeConnectedSockets();
  ASSERT_NE(sockets.listener, nullptr);
  ASSERT_NE(sockets.client, nullptr);
  ASSERT_NE(sockets.server, nullptr);

  droplet::SocketStream stream(sockets.client);
  EXPECT_EQ(stream.readFixSize(droplet::ByteArray::Ptr{}, 1), -1);
  EXPECT_EQ(stream.writeFixSize(droplet::ByteArray::Ptr{}, 1), -1);
}

}  // namespace
