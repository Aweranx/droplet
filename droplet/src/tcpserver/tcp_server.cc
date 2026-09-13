#include "droplet/tcpserver/tcp_server.h"

#include <droplet/config/config.h>
#include <droplet/logger/log.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

namespace droplet {

namespace {

ConfigVar<uint64_t>::Ptr TcpServerRecvTimeout() {
  static auto timeout = Config::Lookup<uint64_t>(
      "tcp_server.read_timeout", TcpServer::kDefaultRecvTimeoutMs,
      "TCP server client receive timeout in milliseconds");
  return timeout;
}

LoggerPtr TcpServerLogger() {
  static auto logger = GetLogger("system");
  return logger;
}

}  // namespace

TcpServer::TcpServer(IOManager* worker, IOManager* io_worker,
                     IOManager* accept_worker)
    : worker_(worker), io_worker_(io_worker), accept_worker_(accept_worker) {
  if (const auto timeout = TcpServerRecvTimeout()) {
    recv_timeout_ms_.store(timeout->getValue(), std::memory_order_relaxed);
  }
}

TcpServer::~TcpServer() {
  std::vector<Socket::Ptr> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners.swap(listeners_);
  }
  closeListeners(std::move(listeners));
}

bool TcpServer::bind(const Address::Ptr& address) {
  std::vector<Address::Ptr> addresses{address};
  std::vector<Address::Ptr> failures;
  return bind(addresses, failures);
}

bool TcpServer::bind(const std::vector<Address::Ptr>& addresses,
                     std::vector<Address::Ptr>& failures) {
  failures.clear();
  if (addresses.empty()) {
    errno = EINVAL;
    return false;
  }
  if (!isStop()) {
    errno = EBUSY;
    return false;
  }

  std::vector<Socket::Ptr> listeners;
  listeners.reserve(addresses.size());
  int last_error = 0;
  for (const auto& address : addresses) {
    if (!address) {
      last_error = EINVAL;
      failures.push_back(address);
      continue;
    }
 
    auto listener = Socket::CreateTCP(address);
    if (!listener || !listener->bind(address) || !listener->listen()) {
      const int error = errno;
      last_error = error;
      DROPLET_LOG_ERROR(TcpServerLogger())
          << "TcpServer bind/listen failed: address=" << address->toString()
          << " errno=" << error << " error=" << std::strerror(error);
      failures.push_back(address);
      continue;
    }
    listeners.push_back(std::move(listener));
  }

  if (!failures.empty()) {
    closeListeners(std::move(listeners));
    errno = last_error;
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopped_.load(std::memory_order_acquire)) {
      closeListeners(std::move(listeners));
      errno = EBUSY;
      return false;
    }
    listeners_.insert(listeners_.end(),
                      std::make_move_iterator(listeners.begin()),
                      std::make_move_iterator(listeners.end()));
  }
  return true;
}

bool TcpServer::start() {
  if (!worker_ || !io_worker_ || !accept_worker_) {
    errno = EINVAL;
    return false;
  }

  auto self = weak_from_this().lock();
  if (!self) {
    errno = EINVAL;
    return false;
  }

  std::vector<Socket::Ptr> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopped_.load(std::memory_order_acquire)) {
      return true;
    }
    if (listeners_.empty()) {
      errno = EINVAL;
      return false;
    }
    stopped_.store(false, std::memory_order_release);
    listeners = listeners_;
  }

  for (const auto& listener : listeners) {
    accept_worker_->schedule([self, listener] { self->startAccept(listener); });
  }
  return true;
}

void TcpServer::stop() {
  const bool was_running = !stopped_.exchange(true, std::memory_order_acq_rel);

  std::vector<Socket::Ptr> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners.swap(listeners_);
  }
  if (listeners.empty()) {
    return;
  }

  // 尚未 start 时没有等待中的 accept Fiber，可以在调用线程直接关闭。
  if (!was_running || !accept_worker_) {
    closeListeners(std::move(listeners));
    return;
  }

  auto self = weak_from_this().lock();
  if (!self || IOManager::GetThis() == accept_worker_) {
    closeListeners(std::move(listeners));
    return;
  }

  // close hook 会在当前 IOManager 中取消监听事件并恢复 accept Fiber。
  accept_worker_->schedule([self, listeners = std::move(listeners)]() mutable {
    self->closeListeners(std::move(listeners));
  });
}

std::string TcpServer::getName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return name_;
}

void TcpServer::setName(std::string name) {
  std::lock_guard<std::mutex> lock(mutex_);
  name_ = std::move(name);
}

std::string TcpServer::getType() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return type_;
}

void TcpServer::setType(std::string type) {
  std::lock_guard<std::mutex> lock(mutex_);
  type_ = std::move(type);
}

std::vector<Socket::Ptr> TcpServer::getSockets() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return listeners_;
}

std::string TcpServer::toString(const std::string& prefix) const {
  std::vector<Socket::Ptr> listeners;
  std::string name;
  std::string type;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners = listeners_;
    name = name_;
    type = type_;
  }

  std::ostringstream output;
  output << prefix << "[type=" << type << " name=" << name
         << " worker=" << (worker_ ? worker_->getName() : "")
         << " io_worker=" << (io_worker_ ? io_worker_->getName() : "")
         << " accept_worker="
         << (accept_worker_ ? accept_worker_->getName() : "")
         << " recv_timeout=" << getRecvTimeout() << " stopped=" << isStop()
         << ']';
  const std::string child_prefix = prefix + "  ";
  for (const auto& listener : listeners) {
    output << '\n' << child_prefix;
    if (listener) {
      output << *listener;
    } else {
      output << "<null socket>";
    }
  }
  return output.str();
}

void TcpServer::handleClient(Socket::Ptr client) {
  if (client) {
    DROPLET_LOG_INFO(TcpServerLogger())
        << "TcpServer handleClient: " << *client;
  }
}

void TcpServer::startAccept(Socket::Ptr listener) {
  while (!isStop() && listener && listener->isValid()) {
    Socket::Ptr client = listener->accept();
    if (client) {
      const uint64_t timeout = getRecvTimeout();
      const int64_t socket_timeout =
          timeout == UINT64_MAX
              ? -1
              : static_cast<int64_t>(std::min<uint64_t>(
                    timeout, static_cast<uint64_t>(
                                 std::numeric_limits<int64_t>::max())));
      client->setRecvTimeout(socket_timeout);

      auto self = shared_from_this();
      io_worker_->schedule([self, client = std::move(client)]() mutable {
        self->handleClient(std::move(client));
      });
      continue;
    }

    const int error = errno;
    if (isStop() || error == EBADF || error == EINVAL) {
      break;
    }
    DROPLET_LOG_ERROR(TcpServerLogger())
        << "TcpServer accept failed: fd=" << listener->getSocket()
        << " errno=" << error << " error=" << std::strerror(error);

    // 资源耗尽等持续错误不能形成忙循环；调度线程中 usleep 会被 hook。
    (void)::usleep(10 * 1000);
  }
}

void TcpServer::closeListeners(std::vector<Socket::Ptr> listeners) {
  for (auto& listener : listeners) {
    if (!listener) {
      continue;
    }
    (void)listener->close();
  }
}

}  // namespace droplet
