#include <droplet/socket/address.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace {

using droplet::Address;
using droplet::IPAddress;
using droplet::IPv4Address;
using droplet::IPv6Address;
using droplet::UnixAddress;
using droplet::UnknownAddress;

TEST(AddressTest, IPv4FormattingAndNetworkOperations) {
  auto address = IPv4Address::Create("192.168.1.42", 8080);
  ASSERT_NE(address, nullptr);
  EXPECT_EQ(address->getFamily(), AF_INET);
  EXPECT_EQ(address->toString(), "192.168.1.42:8080");
  EXPECT_EQ(address->getPort(), 8080u);

  auto network = address->networkAddress(24);
  auto broadcast = address->broadcastAddress(24);
  auto mask = address->subnetMask(24);
  ASSERT_NE(network, nullptr);
  ASSERT_NE(broadcast, nullptr);
  ASSERT_NE(mask, nullptr);
  EXPECT_EQ(network->toString(), "192.168.1.0:8080");
  EXPECT_EQ(broadcast->toString(), "192.168.1.255:8080");
  EXPECT_EQ(mask->toString(), "255.255.255.0:0");

  address->setPort(1234);
  EXPECT_EQ(address->getPort(), 1234u);
  EXPECT_EQ(address->toString(), "192.168.1.42:1234");
  EXPECT_EQ(address->networkAddress(24)->toString(), "192.168.1.0:1234");
  EXPECT_EQ(IPv4Address::Create("not-an-ip"), nullptr);
  EXPECT_EQ(address->networkAddress(33), nullptr);
}

TEST(AddressTest, IPv4IntegerAndComparison) {
  IPv4Address first(INADDR_LOOPBACK, 80);
  IPv4Address same(INADDR_LOOPBACK, 80);
  IPv4Address different(INADDR_LOOPBACK, 81);

  EXPECT_EQ(first, same);
  EXPECT_NE(first, different);
  EXPECT_LT(first, different);
  EXPECT_EQ(first.toString(), "127.0.0.1:80");

  std::vector<Address::Ptr> addresses;
  addresses.push_back(std::make_shared<IPv4Address>(different));
  addresses.push_back(std::make_shared<IPv4Address>(first));
  EXPECT_TRUE(addresses[0] != addresses[1]);
}

TEST(AddressTest, IPv6FormattingAndPrefixOperations) {
  auto address = IPv6Address::Create("2001:db8:1:2::42", 443);
  ASSERT_NE(address, nullptr);
  EXPECT_EQ(address->getFamily(), AF_INET6);
  EXPECT_EQ(address->toString(), "[2001:db8:1:2::42]:443");

  auto network = address->networkAddress(64);
  auto broadcast = address->broadcastAddress(64);
  auto mask = address->subnetMask(64);
  ASSERT_NE(network, nullptr);
  ASSERT_NE(broadcast, nullptr);
  ASSERT_NE(mask, nullptr);
  EXPECT_EQ(network->toString(), "[2001:db8:1:2::]:443");
  EXPECT_EQ(broadcast->toString(), "[2001:db8:1:2:ffff:ffff:ffff:ffff]:443");
  EXPECT_EQ(mask->toString(), "[ffff:ffff:ffff:ffff::]:0");
  EXPECT_EQ(address->networkAddress(129), nullptr);
  EXPECT_EQ(IPv6Address::Create("2001:::1"), nullptr);
}

TEST(AddressTest, UnixAndUnknownAddresses) {
  UnixAddress unix_address("/tmp/droplet-test.sock");
  EXPECT_EQ(unix_address.getFamily(), AF_UNIX);
  EXPECT_EQ(unix_address.getPath(), "/tmp/droplet-test.sock");
  EXPECT_EQ(unix_address.toString(), "/tmp/droplet-test.sock");
  EXPECT_EQ(unix_address.getAddrLen(),
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                   std::string("/tmp/droplet-test.sock").size() +
                                   1));

  std::string abstract_path(1, '\0');
  abstract_path += "droplet-test";
  UnixAddress abstract_address(abstract_path);
  EXPECT_EQ(abstract_address.toString(), "\\0droplet-test");
  EXPECT_EQ(abstract_address.getPath(), abstract_path);

  sockaddr_storage raw{};
  raw.ss_family = AF_PACKET;
  auto unknown = std::make_shared<UnknownAddress>(
      *reinterpret_cast<const sockaddr*>(&raw), sizeof(sockaddr));
  EXPECT_EQ(unknown->getFamily(), AF_PACKET);
  EXPECT_EQ(unknown->toString(), "[UnknownAddress family=17]");
  EXPECT_THROW(UnixAddress(std::string(sizeof(sockaddr_un::sun_path), 'x')),
               std::length_error);
}

TEST(AddressTest, CreateAndLookupHostAddresses) {
  sockaddr_in raw{};
  raw.sin_family = AF_INET;
  raw.sin_port = htons(8080);
  ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &raw.sin_addr), 1);
  auto created = Address::Create(reinterpret_cast<sockaddr*>(&raw),
                                 sizeof(raw));
  ASSERT_NE(created, nullptr);
  EXPECT_EQ(created->toString(), "127.0.0.1:8080");
  EXPECT_NE(std::dynamic_pointer_cast<IPv4Address>(created), nullptr);

  std::vector<Address::Ptr> results;
  ASSERT_TRUE(Address::Lookup(results, "127.0.0.1:8080", AF_INET,
                              SOCK_STREAM, IPPROTO_TCP));
  ASSERT_FALSE(results.empty());
  auto looked_up = std::dynamic_pointer_cast<IPv4Address>(results.front());
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(looked_up->getPort(), 8080u);

  auto any = Address::LookupAny("127.0.0.1:9090");
  ASSERT_NE(any, nullptr);
  EXPECT_EQ(any->toString(), "127.0.0.1:9090");
  auto any_ip = Address::LookupAnyIPAddress("127.0.0.1:9090");
  ASSERT_NE(any_ip, nullptr);
  EXPECT_EQ(any_ip->getPort(), 9090u);
  EXPECT_FALSE(Address::Lookup(results, "this-host-does-not-exist.invalid"));
}

TEST(AddressTest, InterfaceAddressesExposePrefixLength) {
  std::multimap<std::string, std::pair<Address::Ptr, uint32_t>> all;
  ASSERT_TRUE(Address::GetInterfaceAddresses(all, AF_INET));
  if (all.empty()) {
    GTEST_SKIP() << "the test environment has no IPv4 interface";
  }
  for (const auto& [name, value] : all) {
    const auto& [address, prefix] = value;
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(address->getFamily(), AF_INET);
    EXPECT_LE(prefix, 32u);
    EXPECT_FALSE(name.empty());
  }

  std::vector<std::pair<Address::Ptr, uint32_t>> loopback;
  if (Address::GetInterfaceAddresses(loopback, "lo", AF_INET)) {
    ASSERT_FALSE(loopback.empty());
    EXPECT_EQ(loopback.front().first->toString(), "127.0.0.1:0");
    EXPECT_EQ(loopback.front().second, 8u);
  }
}

}  // namespace
