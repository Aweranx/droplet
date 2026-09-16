#include <droplet/fiber/fiber.h>
#include <droplet/types.h>
#include <droplet/scheduler/scheduler.h>
#include <droplet/utils/thread_utils.h>
#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using droplet::Fiber;
using droplet::GetThreadId;
using droplet::Scheduler;

// ---------------------------------------------------------------------------
// 基本任务调度
// ---------------------------------------------------------------------------

TEST(TestScheduler, function_tasks_drained_by_stop) {
  // use_caller=false：start 立即返回，stop 负责排空队列后结束。
  Scheduler sc(3, false, "s-basic");
  std::atomic<int> count{0};
  for (int i = 0; i < 100; ++i) {
    sc.schedule([&count] { ++count; });
  }
  sc.start();
  sc.stop();
  EXPECT_EQ(count.load(), 100);
}

TEST(TestScheduler, use_caller_blocks_until_stop) {
  // use_caller=true：start 在调用线程上进入调度循环并阻塞到 stop；
  // 队列在 start 前投递，stop 后全部执行完毕。
  std::atomic<int> count{0};
  std::atomic<bool> first_task_ran{false};
  Scheduler sc(2, true, "s-caller");

  for (int i = 0; i < 50; ++i) {
    sc.schedule([&count] { ++count; });
  }
  sc.schedule([&first_task_ran] { first_task_ran = true; });

  std::thread stopper([&sc, &first_task_ran] {
    while (!first_task_ran.load()) {
      std::this_thread::yield();  // 等调度循环真正跑起来
    }
    sc.stop();
  });
  sc.start();  // 阻塞，stop 后才返回
  stopper.join();

  EXPECT_EQ(count.load(), 50);
}

TEST(TestScheduler, get_this_inside_task) {
  Scheduler sc(2, false, "s-getthis");
  sc.start();
  std::atomic<bool> ok{false};
  sc.schedule([&sc, &ok] { ok = (Scheduler::GetThis() == &sc); });
  sc.stop();
  EXPECT_TRUE(ok.load());
  EXPECT_EQ(sc.getName(), "s-getthis");
}

// ---------------------------------------------------------------------------
// Fiber 任务与状态机联动
// ---------------------------------------------------------------------------

TEST(TestScheduler, yield_to_ready_auto_requeue) {
  // yield_to_ready 的协程会被调度器自动重新入队，直到自然结束。
  Scheduler sc(2, false, "s-ready");
  std::atomic<int> runs{0};
  auto f = Fiber::Create([&runs] {
    for (int i = 0; i < 3; ++i) {
      ++runs;
      if (i < 2) {
        Fiber::yield_to_ready();
      }
    }
  });
  sc.start();
  sc.schedule(f);
  sc.stop();
  EXPECT_EQ(runs.load(), 3);
  EXPECT_EQ(f->getState(), Fiber::State::TERM);
}

TEST(TestScheduler, yield_to_hold_dropped_until_manual_resume) {
  // yield_to_hold 的协程调度器不再自动调度：状态保持 HOLD，
  // 由外部持有者 resume。
  Scheduler sc(1, false, "s-hold");
  std::atomic<int> runs{0};
  auto f = Fiber::Create([&runs] {
    ++runs;
    Fiber::yield_to_hold();
    ++runs;
  });
  sc.start();
  sc.schedule(f);
  sc.stop();
  EXPECT_EQ(runs.load(), 1);
  EXPECT_EQ(f->getState(), Fiber::State::HOLD);

  f->resume();  // 外部恢复，从 yield 点继续
  EXPECT_EQ(runs.load(), 2);
  EXPECT_EQ(f->getState(), Fiber::State::TERM);
}

TEST(TestScheduler, nested_schedule_from_fiber) {
  // 协程运行中再向调度器投递新任务，stop 前应全部执行完。
  Scheduler sc(2, false, "s-nested");
  std::atomic<int> count{0};
  sc.start();
  sc.schedule([&sc, &count] {
    ++count;
    sc.schedule([&sc, &count] {
      ++count;
      sc.schedule([&count] { ++count; });
    });
  });
  sc.stop();
  EXPECT_EQ(count.load(), 3);
}

// ---------------------------------------------------------------------------
// 线程绑定
// ---------------------------------------------------------------------------

TEST(TestScheduler, pinned_thread_execution) {
  Scheduler sc(3, false, "s-pin");
  sc.start();  // start 返回时全部线程 id 已注册

  const auto& ids = sc.getThreadIds();
  ASSERT_EQ(ids.size(), 3u);

  std::atomic<u64> ran_on{0};
  sc.schedule([&ran_on] { ran_on = GetThreadId(); },
              static_cast<i64>(ids[0]));
  sc.stop();
  EXPECT_EQ(ran_on.load(), ids[0]);
}

TEST(TestScheduler, caller_thread_is_registered) {
  // use_caller 时调用线程在 thread_ids 表里，可被 pin。
  const auto caller_tid = GetThreadId();
  Scheduler sc(2, true, "s-caller-pin");
  std::atomic<u64> ran_on{0};
  std::atomic<bool> first_ran{false};
  sc.schedule([&first_ran] { first_ran = true; });
  sc.schedule([&ran_on] { ran_on = GetThreadId(); },
              static_cast<i64>(caller_tid));

  std::thread stopper([&sc, &first_ran] {
    while (!first_ran.load()) {
      std::this_thread::yield();
    }
    sc.stop();
  });
  sc.start();
  stopper.join();
  EXPECT_EQ(ran_on.load(), caller_tid);
}

// ---------------------------------------------------------------------------
// 批量与压力
// ---------------------------------------------------------------------------

TEST(TestScheduler, batch_schedule) {
  Scheduler sc(2, false, "s-batch");
  std::atomic<int> count{0};
  std::vector<std::function<void()>> tasks;
  for (int i = 0; i < 20; ++i) {
    tasks.emplace_back([&count] { ++count; });
  }
  sc.start();
  sc.schedule(tasks.begin(), tasks.end());
  sc.stop();
  EXPECT_EQ(count.load(), 20);
}

TEST(TestScheduler, many_tasks_stress) {
  Scheduler sc(4, false, "s-stress");
  std::atomic<int> count{0};
  for (int i = 0; i < 1000; ++i) {
    sc.schedule([&count] { ++count; });
  }
  sc.start();
  sc.stop();
  EXPECT_EQ(count.load(), 1000);
}

TEST(TestScheduler, fiber_exception_does_not_kill_thread) {
  // 协程里抛异常转 EXCEPT，调度线程继续工作。
  Scheduler sc(2, false, "s-exception");
  std::atomic<int> count{0};
  auto bad = Fiber::Create([] { throw std::runtime_error("boom"); });
  auto good = Fiber::Create([&count] { ++count; });
  sc.start();
  sc.schedule(bad);
  sc.schedule(good);
  sc.stop();
  EXPECT_EQ(bad->getState(), Fiber::State::EXCEPT);
  EXPECT_EQ(good->getState(), Fiber::State::TERM);
  EXPECT_EQ(count.load(), 1);
}

TEST(TestScheduler, stop_is_idempotent) {
  Scheduler sc(1, false, "s-idempotent");
  sc.start();
  sc.stop();
  sc.stop();  // 第二次调用直接返回
  EXPECT_EQ(Scheduler::GetThis(), nullptr);
}

}  // namespace
