#include <droplet/iomanager/iomanager.h>
#include <droplet/socket/address.h>
#include <droplet/stream/socket_stream.h>
#include <droplet/tcpserver/tcp_server.h>
#include <pthread.h>
#include <signal.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

class EchoServer final : public droplet::TcpServer {
 public:
  using TcpServer::TcpServer;

 protected:
  void handleClient(droplet::Socket::Ptr client) override {
    droplet::SocketStream stream(std::move(client));
    std::array<char, 4096> buffer{};
    while (true) {
      const int count = stream.read(buffer.data(), buffer.size());
      if (count <= 0) {
        break;
      }
      if (stream.writeFixSize(buffer.data(), static_cast<std::size_t>(count)) !=
          count) {
        break;
      }
    }
  }
};

bool ParsePort(std::string_view text, uint16_t& port) {
  unsigned value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      value > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 8080;
  if (argc > 2 || (argc == 2 && !ParsePort(argv[1], port))) {
    std::println(stderr, "usage: {} [port]", argv[0]);
    return 1;
  }

  // 在创建 worker 前屏蔽信号；新线程会继承信号掩码，主线程用 sigwait
  // 同步等待 Ctrl-C，避免在异步信号处理器中调用非信号安全的 C++ 代码。
  sigset_t signals;
  ::sigemptyset(&signals);
  ::sigaddset(&signals, SIGINT);
  ::sigaddset(&signals, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    std::println(stderr, "failed to block SIGINT/SIGTERM");
    return 1;
  }

  droplet::IOManager io_worker(2, false, "echo-io");
  droplet::IOManager accept_worker(1, false, "echo-accept");
  io_worker.start();
  accept_worker.start();

  auto server =
      std::make_shared<EchoServer>(&io_worker, &io_worker, &accept_worker);
  server->setName("droplet-echo");
  auto address = droplet::IPv4Address::Create("0.0.0.0", port);
  if (!address || !server->bind(address) || !server->start()) {
    std::println(stderr, "failed to start TCP echo server on port {}", port);
    server->stop();
    accept_worker.stop();
    io_worker.stop();
    return 1;
  }

  const auto listeners = server->getSockets();
  const auto local_address =
      listeners.empty() ? nullptr : listeners.front()->getLocalAddress();
  std::println("TCP echo server listening on {}",
               local_address ? local_address->toString() : address->toString());
  std::println("Press Ctrl-C to stop.");

  int signal = 0;
  (void)::sigwait(&signals, &signal);
  std::println("received signal {}, stopping...", signal);

  server->stop();
  accept_worker.stop();
  io_worker.stop();
  return 0;
}
