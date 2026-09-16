#include <droplet/http/http_connection.h>
#include <droplet/scheduler/iomanager.h>

#include <exception>
#include <future>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace {

void PrintUsage(const char *program) {
  std::println(stderr, "usage: {} <url> [--post <body>]", program);
  std::println(stderr, "example: {} http://127.0.0.1:8080/hello", program);
  std::println(stderr, "example: {} http://127.0.0.1:8080/echo --post payload",
               program);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 4) {
    PrintUsage(argv[0]);
    return 1;
  }

  const std::string url = argv[1];
  const bool post = argc == 4;
  const std::string body = post ? argv[3] : std::string{};
  if (post && std::string_view(argv[2]) != "--post") {
    PrintUsage(argv[0]);
    return 1;
  }

  // 在 IOManager 的 Fiber 中执行请求，让 socket hook 把等待转换为协程让出。
  droplet::IOManager io(1, false, "http-client");
  io.start();

  std::promise<droplet::http::HttpResult::Ptr> result_promise;
  auto result_future = result_promise.get_future();
  io.schedule([&result_promise, url, body, post] {
    try {
      droplet::http::HttpMap headers;
      if (post) {
        headers.emplace("Content-Type", "text/plain; charset=utf-8");
      }
      auto result =
          post ? droplet::http::HttpConnection::DoPost(url, 5000, headers, body)
               : droplet::http::HttpConnection::DoGet(url, 5000, headers);
      result_promise.set_value(std::move(result));
    } catch (...) {
      result_promise.set_exception(std::current_exception());
    }
  });

  droplet::http::HttpResult::Ptr result;
  try {
    result = result_future.get();
  } catch (const std::exception &exception) {
    std::println(stderr, "HTTP client task failed: {}", exception.what());
    io.stop();
    return 1;
  }
  io.stop();

  if (!result || !result->success() || !result->response) {
    std::println(stderr, "HTTP request failed: {}",
                 result ? result->error : "no result");
    return 1;
  }

  const auto &response = *result->response;
  std::println("HTTP status: {} {}",
               static_cast<unsigned int>(response.getStatus()),
               droplet::http::HttpStatusToString(response.getStatus()));
  for (const auto &[name, value] : response.getHeaders()) {
    std::println("{}: {}", name, value);
  }
  for (const auto &cookie : response.getCookies()) {
    std::println("Set-Cookie: {}", cookie);
  }
  std::println("\n{}", response.getBody());
  return 0;
}
