#include <droplet/config/config.h>
#include <droplet/fiber/fiber.h>
#include <droplet/logger/log.h>
#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using droplet::Config;
using droplet::Fiber;

// ---------------------------------------------------------------------------
// 主协程
// ---------------------------------------------------------------------------

TEST(TestFiber, main_fiber) {
  auto main_fiber = Fiber::GetThis();
  ASSERT_NE(main_fiber, nullptr);
  // 主协程代表线程自身栈：无独立栈、始终 EXEC，且稳定返回同一对象。
  EXPECT_EQ(main_fiber->getState(), Fiber::State::EXEC);
  EXPECT_EQ(main_fiber->getStackSize(), 0u);
  EXPECT_EQ(Fiber::GetThis().get(), main_fiber.get());
  EXPECT_EQ(Fiber::GetFiberId(), main_fiber->getId());
}

// ---------------------------------------------------------------------------
// 基本执行流
// ---------------------------------------------------------------------------

TEST(TestFiber, run_to_term) {
  bool ran = false;
  auto f = Fiber::Create([&] { ran = true; });
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->getState(), Fiber::State::INIT);
  EXPECT_GT(f->getStackSize(), 0u);
  EXPECT_NE(f->getId(), 0u);

  f->resume();
  EXPECT_TRUE(ran);
  EXPECT_EQ(f->getState(), Fiber::State::TERM);

  // 已结束的协程不允许再次 resume。
  EXPECT_DEATH(f->resume(), "");
}

TEST(TestFiber, get_this_identity) {
  Fiber::Ptr inside;
  uint64_t inside_id = 0;
  auto f = Fiber::Create([&] {
    inside = Fiber::GetThis();
    inside_id = Fiber::GetFiberId();
    // 协程内部读自己的状态应为 EXEC。
    EXPECT_EQ(inside->getState(), Fiber::State::EXEC);
  });
  f->resume();
  // 协程内 GetThis() 与外部持有的 Ptr 指向同一对象。
  EXPECT_EQ(inside.get(), f.get());
  EXPECT_EQ(inside_id, f->getId());
  EXPECT_EQ(f->getState(), Fiber::State::TERM);
}

TEST(TestFiber, pingpong_yield_hold) {
  // 主协程与子协程交替执行，验证 yield/resume 的来回切换与状态变化。
  std::vector<int> order;
  auto f = Fiber::Create([&] {
    for (int i = 0; i < 3; ++i) {
      order.push_back(i);
      Fiber::yield_to_hold();
    }
  });

  order.push_back(-1);  // 主协程先执行
  f->resume();
  EXPECT_EQ(f->getState(), Fiber::State::HOLD);
  order.push_back(-1);
  f->resume();
  EXPECT_EQ(f->getState(), Fiber::State::HOLD);
  order.push_back(-1);
  f->resume();
  // 第三轮 yield 后协程仍停在 yield 点，是 HOLD 而不是 TERM。
  EXPECT_EQ(f->getState(), Fiber::State::HOLD);
  order.push_back(-1);
  f->resume();
  // 第四次恢复从 yield 点继续，循环结束，协程函数返回 → TERM。
  EXPECT_EQ(f->getState(), Fiber::State::TERM);

  // 序列：主协程 -1 -> 协程 0 -> 主协程 -1 -> 协程 1 -> ...
  const std::vector<int> expected{-1, 0, -1, 1, -1, 2, -1};
  EXPECT_EQ(order, expected);
}

TEST(TestFiber, yield_to_ready) {
  // resume 从 yield 点继续而不是重跑整个函数：用循环让协程多次让出。
  int runs = 0;
  auto f = Fiber::Create([&] {
    for (int i = 0; i < 3; ++i) {
      ++runs;
      if (i < 2) {
        Fiber::yield_to_ready();
      }
    }
  });
  f->resume();
  EXPECT_EQ(f->getState(), Fiber::State::READY);
  f->resume();
  EXPECT_EQ(f->getState(), Fiber::State::READY);
  f->resume();
  EXPECT_EQ(f->getState(), Fiber::State::TERM);
  EXPECT_EQ(runs, 3);
}

TEST(TestFiber, nested_resume) {
  // 协程 A 内部再 resume 协程 B：验证切换链 A -> B -> A -> 主协程。
  std::vector<std::string> order;
  Fiber::Ptr b;
  auto a = Fiber::Create([&] {
    order.emplace_back("A1");
    b->resume();
    order.emplace_back("A2");
  });
  b = Fiber::Create([&] {
    order.emplace_back("B1");
    Fiber::yield_to_hold();
    order.emplace_back("B2");
  });

  a->resume();
  // B 在 A 的 resume 点让出回到 A，A 结束后回到主协程。
  EXPECT_EQ(order, (std::vector<std::string>{"A1", "B1", "A2"}));
  EXPECT_EQ(a->getState(), Fiber::State::TERM);
  EXPECT_EQ(b->getState(), Fiber::State::HOLD);

  // B 仍可由主协程恢复收尾。
  b->resume();
  EXPECT_EQ(order, (std::vector<std::string>{"A1", "B1", "A2", "B2"}));
  EXPECT_EQ(b->getState(), Fiber::State::TERM);
}

// ---------------------------------------------------------------------------
// 异常与重置
// ---------------------------------------------------------------------------

TEST(TestFiber, exception_to_except_state) {
  auto f = Fiber::Create([] { throw std::runtime_error("boom"); });
  // 异常不跨协程栈传播：resume 正常返回，协程进入 EXCEPT。
  f->resume();
  EXPECT_EQ(f->getState(), Fiber::State::EXCEPT);
}

TEST(TestFiber, reset_reuse_stack) {
  int n = 0;
  auto f = Fiber::Create([&] { ++n; });
  const auto old_stack_size = f->getStackSize();
  f->resume();
  ASSERT_EQ(f->getState(), Fiber::State::TERM);

  // reset 复用同一块栈执行新函数。
  f->reset([&] { n += 10; });
  EXPECT_EQ(f->getState(), Fiber::State::INIT);
  EXPECT_EQ(f->getStackSize(), old_stack_size);
  f->resume();
  EXPECT_EQ(n, 11);
  EXPECT_EQ(f->getState(), Fiber::State::TERM);
}

// ---------------------------------------------------------------------------
// 数量统计与批量运行
// ---------------------------------------------------------------------------

TEST(TestFiber, total_fibers) {
  // 先显式创建主协程：resume 会惰性创建主协程并 +1，
  // 提前创建可让计数断言与测试执行顺序无关。
  auto main_fiber = Fiber::GetThis();
  (void)main_fiber;
  const uint64_t before = Fiber::TotalFibers();
  auto f = Fiber::Create([] {});
  EXPECT_EQ(Fiber::TotalFibers(), before + 1);
  {
    auto g = Fiber::Create([] {});
    EXPECT_EQ(Fiber::TotalFibers(), before + 2);
    g->resume();
  }
  // g 析构后计数回落。
  EXPECT_EQ(Fiber::TotalFibers(), before + 1);
  f->resume();
}

TEST(TestFiber, many_fibers_interleaved) {
  // 批量创建并交错执行：每个协程 yield 两次，统计总执行次数。
  constexpr int kCount = 100;
  constexpr int kSteps = 2;
  std::atomic<int> total{0};
  std::vector<Fiber::Ptr> fibers;
  fibers.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    fibers.push_back(Fiber::Create([&] {
      for (int s = 0; s < kSteps; ++s) {
        ++total;
        Fiber::yield_to_hold();
      }
    }));
  }

  // 反复 resume 直到全部 TERM：每个协程 kSteps 次执行 + 最后一次观察到 TERM。
  bool done = false;
  while (!done) {
    done = true;
    for (auto& f : fibers) {
      if (f->getState() != Fiber::State::TERM) {
        done = false;
        f->resume();
      }
    }
  }
  EXPECT_EQ(total.load(), kCount * kSteps);
  EXPECT_EQ(Fiber::GetThis()->getStackSize(), 0u);  // 主协程无栈
}

// ---------------------------------------------------------------------------
// 线程隔离
// ---------------------------------------------------------------------------

TEST(TestFiber, fibers_are_thread_local) {
  // 主协程按线程独立：不同线程的 GetThis() 互不相同。
  Fiber::Ptr main_here = Fiber::GetThis();
  Fiber::Ptr main_other;
  std::thread t([&] { main_other = Fiber::GetThis(); });
  t.join();
  EXPECT_NE(main_other.get(), main_here.get());
  // 主协程 id 固定为 0（与 sylar 一致），因此这里只比较对象身份。
  EXPECT_EQ(main_other->getId(), 0u);
  EXPECT_EQ(main_here->getId(), 0u);

  // 协程在哪个线程上创建和 resume，就在哪个线程上执行。
  std::thread::id created_tid;
  std::thread::id inside_tid;
  Fiber::Ptr f;
  std::thread t2([&] {
    created_tid = std::this_thread::get_id();
    f = Fiber::Create([&] { inside_tid = std::this_thread::get_id(); });
    f->resume();
  });
  t2.join();
  EXPECT_EQ(created_tid, inside_tid);
  EXPECT_EQ(f->getState(), Fiber::State::TERM);
}

// ---------------------------------------------------------------------------
// 配置模块联动
// ---------------------------------------------------------------------------

TEST(TestFiber, stack_size_from_config) {
  // fiber.cc 静态注册了 fiber.stack_size 配置项，可通过配置模块读取并热更。
  auto var = Config::Lookup<uint32_t>("fiber.stack_size");
  ASSERT_NE(var, nullptr);
  EXPECT_EQ(var->getValue(), Fiber::kDefaultStackSize);

  var->setValue(64 * 1024);
  auto f = Fiber::Create([] {});
  EXPECT_EQ(f->getStackSize(), 64u * 1024u);

  // 构造参数显式给值时优先于配置。
  auto g = Fiber::Create([] {}, 32 * 1024);
  EXPECT_EQ(g->getStackSize(), 32u * 1024u);

  // 还原默认值，避免影响其他用例。
  var->setValue(Fiber::kDefaultStackSize);
}

}  // namespace
