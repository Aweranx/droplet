#include <droplet/http/servlet.h>
#include <droplet/types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

namespace {

TEST(ServletDispatchTest, MatchesExactThenGlobThenDefaultServlet) {
  droplet::http::ServletDispatch dispatch("route-test");

  dispatch.addGlobServlet("/api/*",
                          [](auto request, auto response, auto session) {
                            (void)request;
                            (void)session;
                            response->setBody("glob");
                            return i32{20};
                          });
  dispatch.addServlet("/api/users",
                      [](auto request, auto response, auto session) {
                        (void)request;
                        (void)session;
                        response->setBody("exact");
                        return i32{10};
                      });

  auto request = std::make_shared<droplet::http::HttpRequest>();
  auto response = request->createResponse();
  request->setPath("/api/users");
  EXPECT_EQ(dispatch.handle(request, response, nullptr), 10);
  EXPECT_EQ(response->getBody(), "exact");

  request->setPath("/api/groups/7");
  response = request->createResponse();
  EXPECT_EQ(dispatch.handle(request, response, nullptr), 20);
  EXPECT_EQ(response->getBody(), "glob");

  request->setPath("/missing");
  response = request->createResponse();
  EXPECT_EQ(dispatch.handle(request, response, nullptr), 0);
  EXPECT_EQ(response->getStatus(), droplet::http::HttpStatus::NOT_FOUND);
  EXPECT_EQ(response->getHeader("content-type"), "text/html; charset=utf-8");
  EXPECT_NE(response->getBody().find("route-test"), std::string::npos);
}

TEST(ServletDispatchTest, SupportsLookupReplacementAndDeletion) {
  droplet::http::ServletDispatch dispatch;
  auto first = std::make_shared<droplet::http::FunctionServlet>(
      [](auto, auto, auto) { return i32{1}; });
  auto second = std::make_shared<droplet::http::FunctionServlet>(
      [](auto, auto, auto) { return i32{2}; });

  dispatch.addServlet("/exact", first);
  EXPECT_EQ(dispatch.getServlet("/exact"), first);
  dispatch.addServlet("/exact", second);
  EXPECT_EQ(dispatch.getServlet("/exact"), second);
  EXPECT_TRUE(dispatch.delServlet("/exact"));
  EXPECT_FALSE(dispatch.delServlet("/exact"));
  EXPECT_EQ(dispatch.getServlet("/exact"), nullptr);

  dispatch.addGlobServlet("/files/*", first);
  EXPECT_EQ(dispatch.getGlobServlet("/files/*"), first);
  EXPECT_EQ(dispatch.getMatchedServlet("/files/app.js"), first);
  dispatch.addGlobServlet("/files/*", second);
  EXPECT_EQ(dispatch.getMatchedServlet("/files/app.js"), second);
  EXPECT_TRUE(dispatch.delGlobServlet("/files/*"));
  EXPECT_FALSE(dispatch.delGlobServlet("/files/*"));
  EXPECT_EQ(dispatch.getGlobServlet("/files/*"), nullptr);
  EXPECT_EQ(dispatch.getMatchedServlet("/files/app.js"), dispatch.getDefault());
}

TEST(ServletDispatchTest, AllowsReplacingTheDefaultServlet) {
  droplet::http::ServletDispatch dispatch;
  auto fallback = std::make_shared<droplet::http::FunctionServlet>(
      [](auto, auto response, auto) {
        response->setStatus(droplet::http::HttpStatus::SERVICE_UNAVAILABLE);
        return i32{7};
      });
  dispatch.setDefault(fallback);

  auto request = std::make_shared<droplet::http::HttpRequest>();
  request->setPath("/unknown");
  auto response = request->createResponse();
  EXPECT_EQ(dispatch.handle(request, response, nullptr), 7);
  EXPECT_EQ(response->getStatus(),
            droplet::http::HttpStatus::SERVICE_UNAVAILABLE);
}

} // namespace
