# I/O 协程调度器 iomanager

`IOManager` 位于 `droplet/include/droplet/iomanager/iomanager.h`，实现按职责拆分
在 `droplet/src/iomanager/`：`iomanager.cc` 负责生命周期和底层 fd 上下文，
`iomanager_events.cc` 负责事件注册/取消/分发，`iomanager_idle.cc` 负责
`epoll_wait`、eventfd 唤醒和停止判断。它继承 `Scheduler`，并组合
`TimerManager`，在 Linux 上使用 `epoll + eventfd` 等待文件描述符和定时器。
测试位于 `tests/gtest/test_iomanager.cc`。

## 解决的问题

普通 `Scheduler` 没有任务时在条件变量上等待；`IOManager` 把空闲协程改为
等待 I/O 事件：

```text
Fiber addEvent(fd, READ)
       │
       └─ yield_to_hold()
              │
              ▼
       IOManager::idle()
              │ epoll_wait()
              ▼
       fd 就绪 / 定时器到期
              │
              └─ schedule(Fiber 或 callback)
                         │
                         ▼
                    worker resume
```

`eventfd` 是调度器的唤醒 fd。其他线程投递任务、注册更早的定时器或停止
调度器时，会写入 eventfd，使阻塞中的 `epoll_wait` 立即返回。

## 创建和停止

```cpp
#include <droplet/iomanager/iomanager.h>

droplet::IOManager iom(2, /*use_caller=*/false, "io");
iom.start();

// 投递普通任务、注册 I/O 或定时器...

iom.stop();
```

`use_caller=false` 时，`start()` 创建 worker 后返回；`use_caller=true` 时，
调用线程也进入调度循环，`start()` 会阻塞到 `stop()`。

`stop()` 的退出条件是：

```text
Scheduler 已进入停止状态
且没有未处理的 fd 事件
且没有活动定时器
```

因此，仍然等待 I/O 的事件在停止前应调用 `delEvent` 或 `cancelEvent`；
仍然存在的定时器应调用 `Timer::cancel()`。析构函数会清理剩余注册项，
但等待中的 Fiber 不会被自动恢复。

## fd 事件

### 回调模式

```cpp
int pipe_fds[2];
::pipe(pipe_fds);

iom.addEvent(pipe_fds[0], droplet::IOManager::READ, [fd = pipe_fds[0]] {
  char value;
  ::read(fd, &value, 1);
  // 事件回调运行在某个 IOManager worker 上。
});
```

事件参数只能是 `IOManager::READ` 或 `IOManager::WRITE` 之一。事件是一次性
消费的：回调执行前会从 epoll 中删除该事件；如果还要继续监听，需要再次
调用 `addEvent`。

### Fiber 模式

回调为空时，IOManager 保存当前 Fiber，并在事件就绪后恢复它：

```cpp
auto fiber = droplet::Fiber::Create([&] {
  iom.addEvent(fd, droplet::IOManager::READ);
  droplet::Fiber::yield_to_hold();

  char value;
  ::read(fd, &value, 1);
  // 从 yield_to_hold 后继续执行。
});

iom.schedule(fiber);
```

空回调模式必须在有独立栈的 Fiber 中调用；主协程没有可供
`resume()` 的独立栈，因此不能直接使用该模式。

### 删除和取消

```cpp
iom.delEvent(fd, droplet::IOManager::READ);
```

只删除事件，不执行回调，也不恢复 Fiber。

```cpp
iom.cancelEvent(fd, droplet::IOManager::READ);
iom.cancelAll(fd);
```

取消会删除事件，并把对应的回调/Fiber 投递回 Scheduler。`pendingEventCount()`
可用于观察当前尚未完成的事件注册数量。

## 定时器

```cpp
auto timer = iom.addTimer(100, [] {
  // 100 ms 后在调度线程中执行。
});

auto recurring = iom.addTimer(10, [] {
  // 周期执行。
}, true);

recurring->cancel();
```

`TimerManager` 使用 `steady_clock`，避免系统时间调整影响超时。还提供
`refresh()`、`reset(ms, from_now)` 和 `addConditionTimer()`。定时器回调在
`epoll_wait` 返回后被转成 Scheduler 任务，因此不会直接在 epoll 等待代码中
执行用户逻辑。

## 线程安全

- `addEvent`、`delEvent`、`cancelEvent`、`cancelAll` 和定时器操作可以从
  调度线程以外调用；
- fd 上的事件状态由每个 `FdContext` 的互斥锁保护；
- fd 上的 `epoll_ctl` 由 IOManager 统一维护；
- 回调本身仍由用户负责同步共享数据；
- Fiber 必须在创建它的线程上恢复，不能把 Fiber 指针跨线程手动 resume。

## 与 Scheduler 的关系

| 项目 | Scheduler | IOManager |
| --- | --- | --- |
| 空闲等待 | `condition_variable` | `epoll_wait` |
| 外部唤醒 | `cv_.notify_one/all` | `eventfd` |
| HOLD Fiber | 外部持有者恢复 | fd 就绪时自动恢复 |
| 超时处理 | 无 | `TimerManager` |
| 任务执行 | `schedule` | 继承 Scheduler 的 `schedule` |

当前实现是 Linux 版本；其他平台需要替换 epoll/eventfd 后端，保留同一套
`IOManager` 公开接口即可。
