#include <droplet/scheduler/iomanager.h>
#include <droplet/types.h>
#include <droplet/utils/thread_utils.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <future>
#include <string>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

using droplet::Fiber;
using droplet::IOManager;

struct PipePair {
  int read_fd{-1};
  int write_fd{-1};

  PipePair() {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
      throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    read_fd = fds[0];
    write_fd = fds[1];
  }

  ~PipePair() {
    if (read_fd != -1) {
      ::close(read_fd);
    }
    if (write_fd != -1) {
      ::close(write_fd);
    }
  }

  PipePair(const PipePair&) = delete;
  PipePair& operator=(const PipePair&) = delete;
};

constexpr auto kWait = std::chrono::seconds(2);

TEST(TestIOManager, ReadCallbackRunsAndEventIsConsumed) {
  PipePair pipe;
  IOManager iom(2, false, "io-read-callback");
  iom.start();

  std::promise<void> completed;
  auto future = completed.get_future();
  ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ, [&] {
              char value = 0;
              const ssize_t bytes =
                  ::read(pipe.read_fd, &value, sizeof(value));
              (void)bytes;
              completed.set_value();
            }),
            0);
  ASSERT_EQ(iom.pendingEventCount(), 1u);

  const char value = 'x';
  ASSERT_EQ(::write(pipe.write_fd, &value, sizeof(value)), 1);
  EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(iom.pendingEventCount(), 0u);

  iom.stop();
}

TEST(TestIOManager, FiberWithoutCallbackResumesOnRead) {
  PipePair pipe;
  IOManager iom(1, false, "io-fiber-read");
  std::atomic<bool> registered{false};
  std::atomic<int> add_result{-2};
  std::promise<char> received;
  auto future = received.get_future();

  auto fiber = Fiber::Create([&] {
    add_result = iom.addEvent(pipe.read_fd, IOManager::READ);
    registered = true;
    if (add_result == 0) {
      Fiber::yield_to_hold();
      char value = 0;
      const ssize_t bytes = ::read(pipe.read_fd, &value, sizeof(value));
      (void)bytes;
      received.set_value(value);
    }
  });

  iom.start();
  iom.schedule(fiber);
  while (!registered.load()) {
    std::this_thread::yield();
  }
  ASSERT_EQ(add_result.load(), 0);
  const char value = 'f';
  ASSERT_EQ(::write(pipe.write_fd, &value, sizeof(value)), 1);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(future.get(), 'f');

  iom.stop();
  EXPECT_EQ(fiber->getState(), Fiber::State::TERM);
}

TEST(TestIOManager, DeleteEventDoesNotInvokeHandler) {
  PipePair pipe;
  IOManager iom(1, false, "io-delete");
  iom.start();

  std::atomic<int> calls{0};
  ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ,
                         [&] { ++calls; }),
            0);
  ASSERT_TRUE(iom.delEvent(pipe.read_fd, IOManager::READ));
  EXPECT_EQ(iom.pendingEventCount(), 0u);

  const char value = 'd';
  ASSERT_EQ(::write(pipe.write_fd, &value, sizeof(value)), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(calls.load(), 0);

  iom.stop();
}

TEST(TestIOManager, CancelEventInvokesHandler) {
  PipePair pipe;
  IOManager iom(1, false, "io-cancel");
  iom.start();

  std::promise<void> completed;
  auto future = completed.get_future();
  ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ,
                         [&] { completed.set_value(); }),
            0);
  ASSERT_TRUE(iom.cancelEvent(pipe.read_fd, IOManager::READ));
  EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(iom.pendingEventCount(), 0u);

  iom.stop();
}

TEST(TestIOManager, CancelAllInvokesReadAndWriteHandlers) {
  PipePair pipe;
  IOManager iom(2, false, "io-cancel-all");
  iom.start();

  std::atomic<int> calls{0};
  std::promise<void> completed;
  auto future = completed.get_future();
  auto handler = [&] {
    if (calls.fetch_add(1) + 1 == 2) {
      completed.set_value();
    }
  };

  ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ, handler), 0);
  ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::WRITE, handler), 0);
  ASSERT_EQ(iom.pendingEventCount(), 2u);
  ASSERT_TRUE(iom.cancelAll(pipe.read_fd));
  EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(iom.pendingEventCount(), 0u);

  iom.stop();
}

TEST(TestIOManager, TimerRunsOnSchedulerThread) {
  IOManager iom(2, false, "io-timer");
  iom.start();

  std::promise<u64> completed;
  auto future = completed.get_future();
  const auto timer = iom.addTimer(10, [&] {
    completed.set_value(droplet::GetThreadId());
  });
  ASSERT_NE(timer, nullptr);
  EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_NE(future.get(), 0u);
  EXPECT_FALSE(timer->cancel());

  iom.stop();
}

TEST(TestIOManager, RecurringTimerCanBeCancelled) {
  IOManager iom(1, false, "io-recurring-timer");
  iom.start();

  std::atomic<int> calls{0};
  std::promise<void> completed;
  auto future = completed.get_future();
  auto timer = iom.addTimer(5, [&] {
    if (calls.fetch_add(1) + 1 == 3) {
      completed.set_value();
    }
  }, true);
  ASSERT_NE(timer, nullptr);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  ASSERT_TRUE(timer->cancel());
  const int count_after_cancel = calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(calls.load(), count_after_cancel);

  iom.stop();
}

TEST(TestIOManager, TimerCanRefreshAndReset) {
  IOManager iom(1, false, "io-timer-reset");
  iom.start();

  std::promise<void> completed;
  auto future = completed.get_future();
  auto timer = iom.addTimer(100, [&] { completed.set_value(); });
  ASSERT_NE(timer, nullptr);
  EXPECT_TRUE(timer->refresh());
  EXPECT_TRUE(timer->reset(5, true));
  EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);

  iom.stop();
}

TEST(TestIOManager, ConditionTimerSkipsExpiredOwner) {
  IOManager iom(1, false, "io-condition-timer");
  iom.start();

  auto owner = std::make_shared<int>(1);
  std::weak_ptr<void> weak_owner = owner;
  std::atomic<int> calls{0};
  auto timer = iom.addConditionTimer(5, [&] { ++calls; }, weak_owner);
  ASSERT_NE(timer, nullptr);
  owner.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  iom.stop();
  EXPECT_EQ(calls.load(), 0);
  EXPECT_FALSE(timer->cancel());
}

TEST(TestIOManager, ReportsInvalidAndDuplicateEvents) {
  PipePair pipe;
  IOManager iom(1, false, "io-errors");
  iom.start();

  errno = 0;
  EXPECT_EQ(iom.addEvent(pipe.read_fd, IOManager::NONE, [] {}), -1);
  EXPECT_EQ(errno, EINVAL);

  ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ, [] {}), 0);
  errno = 0;
  EXPECT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ, [] {}), -1);
  EXPECT_EQ(errno, EEXIST);
  ASSERT_TRUE(iom.delEvent(pipe.read_fd, IOManager::READ));

  iom.stop();
}

TEST(TestIOManager, GetThisIsVisibleInsideTask) {
  IOManager iom(1, false, "io-get-this");
  iom.start();

  std::promise<bool> completed;
  auto future = completed.get_future();
  iom.schedule([&] { completed.set_value(IOManager::GetThis() == &iom); });
  EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get());

  iom.stop();
}

TEST(TestIOManager, StopDrainsQueuedTasks) {
  IOManager iom(2, false, "io-drain");
  std::atomic<int> calls{0};
  for (int i = 0; i < 100; ++i) {
    iom.schedule([&] { ++calls; });
  }

  iom.start();
  iom.stop();
  EXPECT_EQ(calls.load(), 100);
}

TEST(TestIOManager, DestructorForcesPendingEventsToClose) {
  PipePair pipe;
  {
    IOManager iom(2, false, "io-destructor");
    iom.start();
    ASSERT_EQ(iom.addEvent(pipe.read_fd, IOManager::READ, [] {}), 0);
    // IOManager 析构路径会先让 worker 退出，再清理未触发的事件。
  }
}

}  // namespace
