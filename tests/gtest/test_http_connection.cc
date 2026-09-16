#include <droplet/http/http_connection.h>
#include <droplet/http/http_server.h>
#include <droplet/iomanager/iomanager.h>
#include <droplet/socket/address.h>
#include <droplet/types.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace {

using droplet::http::HttpConnection;
using droplet::http::HttpConnectionPool;
using droplet::http::HttpMethod;
using droplet::http::HttpResult;
using droplet::http::HttpStatus;

class HttpConnectionTest : public testing::Test {
protected:
  void SetUp() override {
    io_worker_.start();
    accept_worker_.start();

    server_ = std::make_shared<droplet::http::HttpServer>(
        true, &io_worker_, &io_worker_, &accept_worker_);
    server_->setName("droplet-http-client-test");
    server_->setRecvTimeout(2000);
    const auto dispatch = server_->getServletDispatch();
    ASSERT_NE(dispatch, nullptr);
    dispatch->addServlet("/echo", [this](auto request, auto response,
                                         auto session) {
      (void)session;
      request_count_.fetch_add(1, std::memory_order_acq_rel);
      response->setHeader("Content-Type", "text/plain");
      response->setBody(
          std::string(droplet::http::HttpMethodToString(request->getMethod())) +
          '|' + request->getQuery() + '|' + request->getBody() + '|' +
          request->getHeader("host"));
      return i32{0};
    });
    dispatch->addServlet(
        "/empty", [this](auto request, auto response, auto session) {
          (void)request;
          (void)session;
          request_count_.fetch_add(1, std::memory_order_acq_rel);
          response->setStatus(HttpStatus::OK);
          return i32{0};
        });

    const auto bind_address = droplet::IPv4Address::Create("127.0.0.1", 0);
    ASSERT_NE(bind_address, nullptr);
    ASSERT_TRUE(server_->bind(bind_address));
    const auto local = std::dynamic_pointer_cast<droplet::IPv4Address>(
        server_->getSockets().front()->getLocalAddress());
    ASSERT_NE(local, nullptr);
    port_ = static_cast<u16>(local->getPort());
    ASSERT_TRUE(server_->start());
  }

  void TearDown() override {
    if (server_) {
      server_->stop();
      server_.reset();
    }
    accept_worker_.stop();
    io_worker_.stop();
  }

  [[nodiscard]] std::string origin() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  droplet::IOManager io_worker_{2, false, "http-client-io"};
  droplet::IOManager accept_worker_{1, false, "http-client-accept"};
  droplet::http::HttpServer::Ptr server_;
  u16 port_{};
  std::atomic<int> request_count_{0};
};

TEST_F(HttpConnectionTest, SendsOneShotGetAndPostRequests) {
  const std::string host = "127.0.0.1:" + std::to_string(port_);
  auto get = HttpConnection::DoGet(origin() + "/echo?page=2", 2000);
  ASSERT_NE(get, nullptr);
  ASSERT_TRUE(get->success()) << get->toString();
  ASSERT_NE(get->response, nullptr);
  EXPECT_EQ(get->response->getStatus(), HttpStatus::OK);
  EXPECT_EQ(get->response->getBody(), "GET|page=2||" + host);

  droplet::http::HttpMap headers{{"X-Test", "client"}};
  auto post = HttpConnection::DoPost(origin() + "/echo?source=test", 2000,
                                     headers, "payload");
  ASSERT_NE(post, nullptr);
  ASSERT_TRUE(post->success()) << post->toString();
  EXPECT_EQ(post->response->getBody(), "POST|source=test|payload|" + host);
  EXPECT_EQ(request_count_.load(std::memory_order_acquire), 2);
}

TEST_F(HttpConnectionTest, ParsesAnEmptyBodyWithoutWaitingForSocketClose) {
  auto pool = HttpConnectionPool::Create(origin(), {}, 1, 60'000, 10);
  ASSERT_NE(pool, nullptr);
  auto result = pool->doGet("/empty", 2000);
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->success()) << result->toString();
  ASSERT_NE(result->response, nullptr);
  EXPECT_TRUE(result->response->getBody().empty());
  EXPECT_FALSE(result->response->isClose());
  EXPECT_EQ(pool->getIdleConnectionCount(), 1u);
}

TEST_F(HttpConnectionTest, HeadDoesNotCorruptTheNextPooledResponse) {
  auto pool = HttpConnectionPool::Create(origin(), {}, 1, 60'000, 10);
  ASSERT_NE(pool, nullptr);

  auto head = pool->doRequest(HttpMethod::HEAD, "/echo?head=1", 2000);
  ASSERT_NE(head, nullptr);
  ASSERT_TRUE(head->success()) << head->toString();
  EXPECT_TRUE(head->response->getBody().empty());
  EXPECT_EQ(pool->getIdleConnectionCount(), 1u);

  // 最后一条请求显式关闭服务端会话，避免测试 teardown 依赖 FIN 调度时序。
  const droplet::http::HttpMap close_header{{"Connection", "close"}};
  auto get = pool->doGet("/echo?after=head", 2000, close_header);
  ASSERT_NE(get, nullptr);
  ASSERT_TRUE(get->success()) << get->toString();
  EXPECT_NE(get->response->getBody().find("GET|after=head|"),
            std::string::npos);
  EXPECT_EQ(pool->getTotalConnectionCount(), 0u);
}

TEST_F(HttpConnectionTest, ReusesAConnectionAndEnforcesPoolCapacity) {
  auto pool = HttpConnectionPool::Create(origin(), {}, 1, 60'000, 10);
  ASSERT_NE(pool, nullptr);

  auto first = pool->getConnection(2000);
  ASSERT_NE(first, nullptr);
  HttpConnection *const address = first.get();
  EXPECT_EQ(pool->getTotalConnectionCount(), 1u);
  EXPECT_EQ(pool->getConnection(2000), nullptr);

  first.reset();
  EXPECT_EQ(pool->getIdleConnectionCount(), 1u);
  auto second = pool->getConnection(2000);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second.get(), address);
  second.reset();
}

TEST_F(HttpConnectionTest, AConnectionLeaseMayOutliveItsPool) {
  HttpConnection::Ptr lease;
  {
    auto pool = HttpConnectionPool::Create(origin(), {}, 1, 60'000, 10);
    ASSERT_NE(pool, nullptr);
    lease = pool->getConnection(2000);
    ASSERT_NE(lease, nullptr);
    EXPECT_TRUE(lease->isConnected());
  }

  // 租约的删除器只弱引用连接池；池先销毁时，连接由租约自行释放。
  EXPECT_TRUE(lease->isConnected());
  lease.reset();
}

TEST_F(HttpConnectionTest, DropsConnectionsAtThePerConnectionRequestLimit) {
  auto pool = HttpConnectionPool::Create(origin(), {}, 1, 60'000, 1);
  ASSERT_NE(pool, nullptr);

  auto first = pool->doGet("/echo?request=1", 2000);
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(first->success()) << first->toString();
  EXPECT_EQ(pool->getIdleConnectionCount(), 0u);
  EXPECT_EQ(pool->getTotalConnectionCount(), 0u);

  auto second = pool->doGet("/echo?request=2", 2000);
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(second->success()) << second->toString();
  EXPECT_EQ(request_count_.load(std::memory_order_acquire), 2);
}

TEST_F(HttpConnectionTest, ReplacesConnectionsAfterTheirMaximumLifetime) {
  auto pool = HttpConnectionPool::Create(origin(), {}, 1, 100, 10);
  ASSERT_NE(pool, nullptr);

  auto result = pool->doGet("/empty", 2000);
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->success()) << result->toString();
  ASSERT_EQ(pool->getIdleConnectionCount(), 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(125));
  auto replacement = pool->getConnection(2000);
  ASSERT_NE(replacement, nullptr);
  // 新连接尚未完成请求；若错误复用了过期连接，这里会仍然是 1。
  EXPECT_EQ(replacement->getRequestCount(), 0u);
}

TEST(HttpConnectionErrorTest, ReportsInvalidAndUnsupportedUrls) {
  auto invalid = HttpConnection::DoGet("not a URL", 100);
  ASSERT_NE(invalid, nullptr);
  EXPECT_EQ(invalid->result, static_cast<int>(HttpResult::Error::INVALID_URL));

  auto https = HttpConnection::DoGet("https://example.com/", 100);
  ASSERT_NE(https, nullptr);
  EXPECT_EQ(https->result,
            static_cast<int>(HttpResult::Error::UNSUPPORTED_SCHEME));

  auto pool =
      HttpConnectionPool::Create("https://example.com", {}, 1, 60'000, 10);
  ASSERT_NE(pool, nullptr);
  auto pooled = pool->doGet("/", 100);
  ASSERT_NE(pooled, nullptr);
  EXPECT_EQ(pooled->result,
            static_cast<int>(HttpResult::Error::UNSUPPORTED_SCHEME));

  EXPECT_EQ(HttpConnectionPool::Create("broken", {}, 1, 1, 1), nullptr);
  EXPECT_EQ(HttpConnectionPool::Create("http://example.com", {}, 0, 1, 1),
            nullptr);
}

} // namespace
