#include <droplet/http/http.h>
#include <droplet/http/http_parser.h>
#include <droplet/http/http_request.h>
#include <droplet/http/http_response.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

namespace {

using droplet::http::HttpMethod;
using droplet::http::HttpParseError;
using droplet::http::HttpRequestParser;
using droplet::http::HttpStatus;

class HttpMethodConversionTest
    : public testing::TestWithParam<std::tuple<std::string_view, HttpMethod>> {
};

TEST_P(HttpMethodConversionTest, ConvertsInBothDirections) {
  const auto &[name, method] = GetParam();
  EXPECT_EQ(droplet::http::StringToHttpMethod(name), method);
  EXPECT_EQ(droplet::http::CharsToHttpMethod(name.data()), method);
  EXPECT_EQ(droplet::http::HttpMethodToString(method), name);
}

INSTANTIATE_TEST_SUITE_P(
    StandardMethods, HttpMethodConversionTest,
    testing::Values(std::tuple{"GET", HttpMethod::GET},
                    std::tuple{"POST", HttpMethod::POST},
                    std::tuple{"PUT", HttpMethod::PUT},
                    std::tuple{"DELETE", HttpMethod::DELETE},
                    std::tuple{"PATCH", HttpMethod::PATCH},
                    std::tuple{"M-SEARCH", HttpMethod::MSEARCH}));

TEST(HttpDataModelTest, RejectsUnknownOrCaseChangedMethodAndMapsStatusText) {
  // HTTP 方法区分大小写；协议中的标准方法使用大写形式。
  EXPECT_EQ(droplet::http::StringToHttpMethod("get"),
            HttpMethod::INVALID_METHOD);
  EXPECT_EQ(droplet::http::StringToHttpMethod("UNKNOWN"),
            HttpMethod::INVALID_METHOD);
  EXPECT_EQ(droplet::http::CharsToHttpMethod(nullptr),
            HttpMethod::INVALID_METHOD);
  EXPECT_STREQ(droplet::http::HttpMethodToString(HttpMethod::INVALID_METHOD),
               "INVALID_METHOD");
  EXPECT_STREQ(droplet::http::HttpStatusToString(HttpStatus::OK), "OK");
  EXPECT_STREQ(droplet::http::HttpStatusToString(HttpStatus::NOT_FOUND),
               "Not Found");
}

TEST(HttpRequestParserTest, ParsesRequestFromAString) {
  const std::string request_text =
      "POST /api/items?source=unit+test&limit=10#result HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\n"
      "Cookie: session=abc123; theme=dark\r\n"
      "X-Request-Id: 42\r\n"
      "Connection: keep-alive\r\n"
      "Content-Length: 20\r\n"
      "\r\n"
      "value=42&active=true";

  HttpRequestParser parser;
  EXPECT_EQ(parser.execute(request_text), request_text.size());
  ASSERT_TRUE(parser.isFinished()) << parser.getErrorMessage();
  ASSERT_FALSE(parser.hasError());
  ASSERT_NE(parser.getData(), nullptr);

  const auto &request = parser.getData();
  EXPECT_EQ(request->getMethod(), HttpMethod::POST);
  EXPECT_EQ(request->getVersion(), 0x11);
  EXPECT_EQ(request->getPath(), "/api/items");
  EXPECT_EQ(request->getQuery(), "source=unit+test&limit=10");
  EXPECT_EQ(request->getFragment(), "result");
  EXPECT_EQ(request->getBody(), "value=42&active=true");
  EXPECT_FALSE(request->isClose());
  EXPECT_EQ(parser.getContentLength(), 20u);

  // HTTP 头字段名大小写不敏感。
  EXPECT_EQ(request->getHeader("host"), "example.com");
  EXPECT_EQ(request->getHeader("X-REQUEST-ID"), "42");
  EXPECT_EQ(request->getHeaderAs<int>("x-request-id"), 42);

  // 查询字符串和 urlencoded 请求体会合并到参数表中。
  EXPECT_EQ(request->getParam("source"), "unit test");
  EXPECT_EQ(request->getParamAs<int>("limit"), 10);
  EXPECT_EQ(request->getParamAs<int>("value"), 42);
  EXPECT_TRUE(request->getParamAs<bool>("active"));
  EXPECT_EQ(request->getCookie("session"), "abc123");
  EXPECT_EQ(request->getCookie("theme"), "dark");
}

TEST(HttpRequestParserTest, LeavesTheNextPipelinedRequestInTheInputBuffer) {
  const std::string first = "GET /first HTTP/1.1\r\nHost: example.com\r\n\r\n";
  const std::string second = "GET /second HTTP/1.0\r\nHost: "
                             "example.com\r\nConnection: keep-alive\r\n\r\n";
  std::string buffer = first + second;

  HttpRequestParser parser;
  const std::size_t consumed = parser.execute(buffer.data(), buffer.size());
  ASSERT_EQ(consumed, first.size());
  ASSERT_TRUE(parser.isFinished());
  EXPECT_EQ(parser.getData()->getPath(), "/first");

  // execute(char*, size_t) 把未消费部分前移，HttpSession 可直接在其后继续收包。
  buffer.resize(buffer.size() - consumed);
  EXPECT_EQ(buffer, second);

  parser.reset();
  EXPECT_EQ(parser.execute(buffer.data(), buffer.size()), second.size());
  ASSERT_TRUE(parser.isFinished());
  EXPECT_EQ(parser.getData()->getPath(), "/second");
  EXPECT_FALSE(parser.getData()->isClose());
}

TEST(HttpRequestParserTest, WaitsForCompleteHeadersAndBody) {
  HttpRequestParser parser;
  EXPECT_EQ(parser.execute("POST / HTTP/1.1\r\nContent-Length: 5\r\n"), 0u);
  EXPECT_FALSE(parser.isFinished());
  EXPECT_FALSE(parser.hasError());

  const std::string incomplete_body =
      "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nabc";
  EXPECT_EQ(parser.execute(incomplete_body), 0u);
  EXPECT_FALSE(parser.isFinished());
  EXPECT_FALSE(parser.hasError());

  const std::string complete = incomplete_body + "de";
  EXPECT_EQ(parser.execute(complete), complete.size());
  ASSERT_TRUE(parser.isFinished());
  EXPECT_EQ(parser.getData()->getBody(), "abcde");
}

class InvalidHttpRequestTest
    : public testing::TestWithParam<
          std::tuple<std::string_view, HttpParseError>> {};

TEST_P(InvalidHttpRequestTest, ReportsTheSpecificParseError) {
  const auto &[request_text, expected_error] = GetParam();
  HttpRequestParser parser;
  EXPECT_EQ(parser.execute(request_text), 0u);
  EXPECT_TRUE(parser.hasError());
  EXPECT_FALSE(parser.isFinished());
  EXPECT_EQ(parser.getError(), expected_error);
  EXPECT_FALSE(parser.getErrorMessage().empty());
}

INSTANTIATE_TEST_SUITE_P(
    MalformedRequests, InvalidHttpRequestTest,
    testing::Values(
        std::tuple{"BREW /coffee HTTP/1.1\r\nHost: example.com\r\n\r\n",
                   HttpParseError::INVALID_METHOD},
        std::tuple{"GET / HTTP/2.0\r\nHost: example.com\r\n\r\n",
                   HttpParseError::INVALID_VERSION},
        std::tuple{"GET / HTTP/1.1\r\nBroken-Header\r\n\r\n",
                   HttpParseError::INVALID_HEADER},
        std::tuple{"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                   HttpParseError::INVALID_CONTENT_LENGTH},
        std::tuple{
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
            HttpParseError::UNSUPPORTED_TRANSFER_ENCODING}));

TEST(HttpDataModelTest, SerializesRequestAndResponse) {
  droplet::http::HttpRequest request(0x11, false);
  request.setMethod(HttpMethod::POST);
  request.setPath("/messages");
  request.setQuery("draft=1");
  request.setHeader("Host", "example.com");
  request.setHeader("Content-Length", "999");
  request.setBody("hello");

  const std::string serialized_request = request.toString();
  EXPECT_EQ(serialized_request.find("POST /messages?draft=1 HTTP/1.1\r\n"), 0u);
  EXPECT_NE(serialized_request.find("Host: example.com\r\n"),
            std::string::npos);
  EXPECT_NE(serialized_request.find("Connection: keep-alive\r\n"),
            std::string::npos);
  EXPECT_NE(serialized_request.find("Content-Length: 5\r\n"),
            std::string::npos);
  EXPECT_EQ(serialized_request.find("999"), std::string::npos);
  EXPECT_TRUE(serialized_request.ends_with("\r\n\r\nhello"));

  auto response = request.createResponse();
  response->setStatus(HttpStatus::CREATED);
  response->setHeader("Content-Type", "text/plain");
  response->setBody("created");
  response->setCookie("session", "abc", 0, "/", {}, true, true, "Lax");

  const std::string serialized_response = response->toString();
  EXPECT_EQ(serialized_response.find("HTTP/1.1 201 Created\r\n"), 0u);
  EXPECT_NE(serialized_response.find("Content-Type: text/plain\r\n"),
            std::string::npos);
  EXPECT_NE(serialized_response.find("Set-Cookie: session=abc; Path=/; Secure; "
                                     "HttpOnly; SameSite=Lax\r\n"),
            std::string::npos);
  EXPECT_NE(serialized_response.find("Content-Length: 7\r\n"),
            std::string::npos);
  EXPECT_TRUE(serialized_response.ends_with("\r\n\r\ncreated"));
}

} // namespace
