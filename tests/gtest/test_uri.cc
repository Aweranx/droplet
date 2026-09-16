#include <droplet/socket/address.h>
#include <droplet/uri/uri.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <tuple>

namespace {

TEST(UriTest, ParsesAndSerializesAnAbsoluteHttpUri) {
  const auto uri = droplet::Uri::Create(
      "HTTP://user@example.com:8080/api/items?q=42#result");
  ASSERT_NE(uri, nullptr);
  EXPECT_EQ(uri->getScheme(), "http");
  EXPECT_EQ(uri->getUserinfo(), "user");
  EXPECT_EQ(uri->getHost(), "example.com");
  EXPECT_EQ(uri->getPort(), 8080);
  EXPECT_EQ(uri->getPath(), "/api/items");
  EXPECT_EQ(uri->getQuery(), "q=42");
  EXPECT_EQ(uri->getFragment(), "result");
  EXPECT_FALSE(uri->isDefaultPort());
  EXPECT_EQ(uri->toString(),
            "http://user@example.com:8080/api/items?q=42#result");
}

TEST(UriTest, AppliesDefaultPortsAndBracketsIpv6WhenSerializing) {
  const auto default_http = droplet::Uri::Create("http://example.com");
  ASSERT_NE(default_http, nullptr);
  EXPECT_EQ(default_http->getPort(), 80);
  EXPECT_TRUE(default_http->isDefaultPort());
  EXPECT_EQ(default_http->getPath(), "/");
  EXPECT_EQ(default_http->toString(), "http://example.com/");

  const auto ipv6 = droplet::Uri::Create("https://[::1]:8443/status");
  ASSERT_NE(ipv6, nullptr);
  EXPECT_EQ(ipv6->getHost(), "::1");
  EXPECT_EQ(ipv6->getPort(), 8443);
  EXPECT_EQ(ipv6->toString(), "https://[::1]:8443/status");
}

TEST(UriTest, SupportsRelativeReferences) {
  const auto uri = droplet::Uri::Create("../items?page=2#top");
  ASSERT_NE(uri, nullptr);
  EXPECT_TRUE(uri->getScheme().empty());
  EXPECT_FALSE(uri->hasAuthority());
  EXPECT_EQ(uri->getPort(), -1);
  EXPECT_EQ(uri->getPath(), "../items");
  EXPECT_EQ(uri->toString(), "../items?page=2#top");
}

class InvalidUriTest : public testing::TestWithParam<std::string> {};

TEST_P(InvalidUriTest, RejectsMalformedUri) {
  EXPECT_EQ(droplet::Uri::Create(GetParam()), nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    MalformedUris, InvalidUriTest,
    testing::Values("1http://example.com/",
                    "http://example.com:", "http://example.com:65536/",
                    "http://[::1/", "http://::1/", "http://example.com/bad%2G",
                    "http://example.com/has space"));

TEST(UriTest, ResolvesHostAndAppliesThePort) {
  const auto uri = droplet::Uri::Create("http://127.0.0.1:32123/path");
  ASSERT_NE(uri, nullptr);
  const auto address = uri->createAddress();
  ASSERT_NE(address, nullptr);
  EXPECT_EQ(address->getPort(), 32123u);
  EXPECT_NE(address->toString().find("32123"), std::string::npos);
}

} // namespace
