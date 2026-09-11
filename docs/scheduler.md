# 协程调度模块 scheduler

实现位于 `droplet/src/scheduler/`，按职责拆分为：`scheduler.cc`（构造、析构
和线程本地实例）、`scheduler_lifecycle.cc`（start/stop）、
`scheduler_task.cc`（任务投递和取任务）以及 `scheduler_run.cc`（run/idle
主循环和停止钩子）。

## 定位

回答一个问题：**哪个协程在哪个线程上跑**。它是"线程池 + 协程队列"：
调度线程反复取出任务并 resume，协程用 `yield_to_ready` / `yield_to_hold`
把控制权交还。没有 IO 概念——事件驱动是上层 IOManager 的事。

## 核心概念

- **调度线程**：worker 线程 + （use_caller 时的）调用线程，都跑同一个
  `run()` 主循环。
- **任务**：`ScheduleTask{Fiber::Ptr 或 function, thread_id}`，
  thread_id = -1 表示任意线程，否则 pin 到指定线程。
- **idle 协程**：每个调度线程一个，无任务时在条件变量上睡眠；
  是协程而非普通函数，是为了让 IOManager 能把它重写成 epoll_wait 循环、
  并在有事件时让出回 run 循环。
- **READY / HOLD 的分工**：READY = "我还想跑"，调度器自动重新入队；
  HOLD = "我在等外部事件"，调度器撒手，恢复责任在持有者
  （现在是你自己，IOManager 接管后是 fd 事件）。

## 流程

```
schedule(fiber/cb)                          调度线程 run() 主循环
   │ 上锁入队 + notify_one + tickle()            │
   ▼                                            ▼
tasks_ ──────────────────────────────▶ getNextTask(): 优先本线程 pin 的，
   │                                   其次任意线程；EXEC 协程跳过
   │                                        │ 取到任务
   │                                        ▼
   │                              fiber ? resume() : 包装成协程 resume()
   │                                        │
   │                                        ├─ TERM/EXCEPT → 释放
   │            重新入队（保持 pin）◀────────┼─ READY（yield_to_ready）
   │                                        └─ HOLD → 调度器撒手，
   │                                              恢复责任在持有者
   │                                        │ 没取到任务
   │                                        ▼
   └── notify 唤醒 ◀──── idle 协程：cv.wait(有任务或停止)
                                            │ 停止且队列空
                                            ▼
                                    idle 协程 TERM → run 循环退出
```

## 实现细节

### use_caller：调用线程作为调度线程

构造时 `thread_count` 含调用线程自身（worker 数 = thread_count - 1，
caller 的 id 在构造时即入 `thread_ids_` 表）。`start()` 在创建 worker 后
**直接在调用线程栈上进入 `run()` 循环**，阻塞到 stop。

sylar 用 root fiber + `CallerMainFunc` 包装这层切换；droplet 让循环直接
跑在线程栈上，语义相同（调用线程被调度器占用到 stop）、少一层协程间接。
代价是一条约束：stop 后 caller 线程才从 start() 返回，"start 之后还想在
caller 干活"的场景要改用 use_caller=false。

### stop 的排空语义

`stop()` 置停止位 → notify 全部 idle → join 所有 **worker**（caller 不在
`threads_` 里，不会 join 自己）。正在执行的协程跑完；`yield_to_ready`
再入队的任务照常执行；队列清空后 idle 协程 TERM，各线程自然退出。

两个关键点：

1. **schedule() 允许在停止流程中调用**——run 循环对 READY 协程的重新
   入队就发生在停止期间（第一版在这里加了 `!stopping_` 断言，被
   stop 与协程执行的并发竞态打爆，5 次跑挂 3 次，教训：排空语义要求
   停止中仍可入队）。停止后投递的任务可能不被执行，接口注释已声明。
2. **stop 不能在 worker 线程上调用**（join 自己死锁），Debug 下断言；
   use_caller 时在 caller 线程（含其协程内）调用是合法的。

### 函数任务的协程复用

`schedule(function)` 的函数会包装成协程执行。每个调度线程持有一个
`cb_fiber`，上一个函数跑完后处于 TERM，下一个函数用 `reset()` 复用
同一块栈（fiber 模块的 reset 正好派上用场），避免反复分配/释放 128KB。

注意：普通 `Scheduler` 的函数协程不能在没有外部持有者时
`yield_to_hold`；IOManager 的 `addEvent(cb={})` 会把当前 Fiber 放入事件
上下文，因此可以在等待 fd 时挂起。需要手动控制生命周期的任务仍建议用
`Fiber::Ptr` 投递并自行持有。

### 状态机与调度器的分工（与 sylar 一致）

| 让出方式 | 协程状态 | 调度器行为 | 典型场景 |
| --- | --- | --- | --- |
| yield_to_ready | READY | 重新入队 | 协作式让出时间片 |
| yield_to_hold | HOLD | 不再自动调度 | 等 IO 事件（IOManager 恢复） |
| 回调自然返回 | TERM | 释放 | 任务完成 |

### 启动屏障

`start()` 返回前等待全部调度线程完成 id 注册（`std::latch`），保证之后
`schedule(fc, thread_id)` pin 任何线程都有效，也使 `getThreadIds()`
的读取无竞态。

### 相对 sylar 的差异汇总

| 项 | sylar | droplet |
| --- | --- | --- |
| use_caller 实现 | root fiber + CallerMainFunc 协程包装 | 循环直接跑在调用线程栈上 |
| stop 时的重入队 | 允许（schedule 无停止断言） | 允许（第一版的断言已移除） |
| 唤醒 | stop 前置检查 + tickle 虚函数 | 条件变量 notify + tickle 虚函数 |
| 切换原语 | ucontext | fcontext（见 fiber.md） |
| switchTo | 有 | 未实现（用到再加） |

## 使用示例

```cpp
#include <droplet/scheduler/scheduler.h>

int main() {
  droplet::Scheduler sc(4, /*use_caller=*/true, "main_sched");

  // 投递函数任务
  sc.schedule([] { std::println("task on tid={}", droplet::GetThreadId()); });

  // 投递协程任务：协作式让出会被再次调度
  auto f = droplet::Fiber::Create([] {
    for (int i = 0; i < 3; ++i) {
      std::println("step {}", i);
      droplet::Fiber::yield_to_ready();
    }
  });
  sc.schedule(f);

  // pin 到调度器某条线程
  sc.schedule([] { /* ... */ },
              static_cast<int64_t>(sc.getThreadIds()[0]));

  sc.start();  // use_caller：阻塞在调用线程的调度循环上
  sc.stop();   // （通常由其他线程或信号处理路径调用）
}
```

## 总结

调度器本体很薄：一个带锁的双端任务表 + 每线程同一个 run 循环 + 可重写的
idle 协程。工程量在三处边界：stop 与运行中协程的并发（重入队必须放行）、
use_caller 的线程归属（caller 不进 join 集合）、函数协程的生命周期
（复用 vs HOLD 悬垂）。测试覆盖函数/协程/嵌套/批量/pin 线程/use_caller
阻塞语义/异常不杀线程/1000 任务压力，连续 10 次全量运行稳定通过。

## TODO

- IOManager 已重写 idle（epoll_wait）与 tickle（eventfd），并由 fd 事件
  恢复 HOLD 协程；详见 `docs/iomanager.md`。
- 工作窃取 / 线程本地任务队列，降低全局锁竞争（当前一把全局锁）。
- switchTo / schedule 指定"下次空闲再跑"的惰性投递。
