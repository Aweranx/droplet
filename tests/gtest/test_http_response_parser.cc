#include <droplet/http/http_parser.h>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <tuple>

namespace {

using droplet::http::HttpMethod;
using droplet::http::HttpParseError;
using droplet::http::HttpResponseParser;
using droplet::http::HttpStatus;

TEST(HttpResponseParserTest, ParsesContentLengthAndPreservesPipelinedData) {
  const std::string first = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                            "Connection: keep-alive\r\nConnection: close\r\n"
                            "X-Value: 42\r\n\r\nhello";
  const std::string second =
      "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
  std::string buffer = first + second;

  HttpResponseParser parser;
  const std::size_t consumed = parser.execute(buffer.data(), buffer.size());
  ASSERT_EQ(consumed, first.size());
  ASSERT_TRUE(parser.isFinished()) << parser.getErrorMessage();
  EXPECT_EQ(parser.getData()->getStatus(), HttpStatus::OK);
  EXPECT_EQ(parser.getData()->getBody(), "hello");
  EXPECT_EQ(parser.getData()->getHeaderAs<int>("x-value"), 42);
  // 重复 Connection 头按逗号列表合并，任意 close 都会禁止连接复用。
  EXPECT_TRUE(parser.getData()->isClose());

  buffer.resize(buffer.size() - consumed);
  EXPECT_EQ(buffer, second);
}

TEST(HttpResponseParserTest, DecodesChunksExtensionsAndTrailers) {
  std::string response =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"
      "Connection: keep-alive\r\n\r\n"
      "4;source=test\r\nWiki\r\n5\r\npedia\r\n"
      "0\r\nX-Trailer: done\r\n\r\nNEXT";
  const std::size_t expected_consumed = response.size() - 4;

  HttpResponseParser parser;
  const std::size_t consumed = parser.execute(response.data(), response.size());
  ASSERT_EQ(consumed, expected_consumed);
  ASSERT_TRUE(parser.isFinished()) << parser.getErrorMessage();
  EXPECT_TRUE(parser.isChunked());
  EXPECT_EQ(parser.getContentLength(), 9u);
  EXPECT_EQ(parser.getData()->getBody(), "Wikipedia");
  EXPECT_EQ(parser.getData()->getHeader("x-trailer"), "done");

  response.resize(response.size() - consumed);
  EXPECT_EQ(response, "NEXT");
}

TEST(HttpResponseParserTest, WaitsForEofForCloseDelimitedBody) {
  const std::string response =
      "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nbody-to-eof";
  HttpResponseParser parser;
  EXPECT_EQ(parser.execute(response, false), 0u);
  EXPECT_FALSE(parser.isFinished());
  EXPECT_TRUE(parser.isCloseDelimited());

  EXPECT_EQ(parser.execute(response, true), response.size());
  ASSERT_TRUE(parser.isFinished()) << parser.getErrorMessage();
  EXPECT_EQ(parser.getData()->getBody(), "body-to-eof");
  EXPECT_TRUE(parser.getData()->isClose());
}

TEST(HttpResponseParserTest, HeadAndNoContentResponsesHaveNoBody) {
  const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 123\r\n\r\nNEXT";
  HttpResponseParser head_parser(HttpMethod::HEAD);
  const std::size_t consumed = head_parser.execute(head);
  ASSERT_TRUE(head_parser.isFinished()) << head_parser.getErrorMessage();
  EXPECT_EQ(consumed, head.size() - 4);
  EXPECT_TRUE(head_parser.getData()->getBody().empty());

  const std::string no_content =
      "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n";
  HttpResponseParser no_content_parser;
  EXPECT_EQ(no_content_parser.execute(no_content), no_content.size());
  ASSERT_TRUE(no_content_parser.isFinished());
  EXPECT_TRUE(no_content_parser.getData()->getBody().empty());
  EXPECT_FALSE(no_content_parser.getData()->isClose());
}

TEST(HttpResponseParserTest, AcceptsAResponseAfterMoreBytesArrive) {
  HttpResponseParser parser;
  const std::string partial = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc";
  EXPECT_EQ(parser.execute(partial), 0u);
  EXPECT_FALSE(parser.hasError());

  const std::string complete = partial + "de";
  EXPECT_EQ(parser.execute(complete), complete.size());
  ASSERT_TRUE(parser.isFinished()) << parser.getErrorMessage();
  EXPECT_EQ(parser.getData()->getBody(), "abcde");
}

TEST(HttpResponseParserTest, PreservesMultipleSetCookieHeaders) {
  const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                               "Set-Cookie: first=1; Path=/\r\n"
                               "Set-Cookie: second=2; HttpOnly\r\n\r\n";
  HttpResponseParser parser;
  EXPECT_EQ(parser.execute(response), response.size());
  ASSERT_TRUE(parser.isFinished()) << parser.getErrorMessage();
  ASSERT_EQ(parser.getData()->getCookies().size(), 2u);
  EXPECT_EQ(parser.getData()->getCookies()[0], "first=1; Path=/");
  EXPECT_EQ(parser.getData()->getCookies()[1], "second=2; HttpOnly");
}

class InvalidHttpResponseTest
    : public testing::TestWithParam<
          std::tuple<std::string_view, HttpParseError>> {};

TEST_P(InvalidHttpResponseTest, ReportsTheSpecificParseError) {
  const auto &[text, expected_error] = GetParam();
  HttpResponseParser parser;
  EXPECT_EQ(parser.execute(text), 0u);
  EXPECT_TRUE(parser.hasError());
  EXPECT_EQ(parser.getError(), expected_error);
  EXPECT_FALSE(parser.getErrorMessage().empty());
}

INSTANTIATE_TEST_SUITE_P(
    MalformedResponses, InvalidHttpResponseTest,
    testing::Values(
        std::tuple{"HTTP/2.0 200 OK\r\n\r\n", HttpParseError::INVALID_VERSION},
        std::tuple{"HTTP/1.1 two OK\r\n\r\n", HttpParseError::INVALID_STATUS},
        std::tuple{"HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n",
                   HttpParseError::INVALID_HEADER},
        std::tuple{"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                   HttpParseError::INVALID_CONTENT_LENGTH},
        std::tuple{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                   "\r\nZZ\r\n",
                   HttpParseError::INVALID_CHUNK_SIZE}));

} // namespace
