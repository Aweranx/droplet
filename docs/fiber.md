# 协程模块 fiber
对称协程

## 为什么是 fcontext 而不是 ucontext

| | ucontext（sylar） | fcontext（droplet） |
| --- | --- | --- |
| 切换成本 | ~100-300ns，每次切换陷内核保存/恢复信号掩码 | ~10-20ns，纯用户态保存 callee-saved 寄存器 |
| 上下文对象 | `ucontext_t` 结构体，swapcontext 自动更新 | `void*` 句柄，**新恢复点要从返回值手动取回** |
| 闭包参数 | makecontext 传整数参数（变参黑魔法） | transfer_t.data 指针随切换传递 |
| 协程函数返回 | uc_link 自动跳回 | 没有！普通 return 会落入汇编 finish 桩 `_exit(0)` |
| 栈对齐 | 无要求 | 内部自行对齐 16 字节（分配时按 64 对齐更稳） |

fcontext 剥离自 Boost.Context（BSL-1.0 许可，允许直接拷贝），只取了一个
头文件加两个汇编（本项目限定 x86_64 / SysV ABI / ELF）：

```
droplet/src/fiber/fcontext/
├── fcontext.hpp                    # fcontext_t / transfer_t / 两个原语的声明
├── make_x86_64_sysv_elf_gas.S      # 在新栈上布置初始寄存器保存区 + trampoline
└── jump_x86_64_sysv_elf_gas.S      # 保存当前 callee-saved 寄存器，换栈跳转
```

汇编里保存的只有 rbx/rbp/r12-r15 + mxcsr/x87 控制字（SysV 约定的
callee-saved），XMM 数据寄存器按 ABI 是 caller-saved 不用保存。

## 核心概念

- **Fiber**：协程对象，六态状态机
  `INIT / HOLD / EXEC / TERM / READY / EXCEPT`（与 sylar 相同）。
- **主协程**：代表线程自身调用栈的协程，无独立栈、始终 EXEC、每线程一份、
  id 固定为 0；首次调用 `GetThis()` 时惰性创建，随 thread_local 在线程
  退出时销毁。
- **back_ctx_**：最近一次进入本协程时的"恢复者上下文"，yield 和结束收尾
  都跳回它。

## 流程

```
主协程                                     子协程（stack: aligned_alloc）
   │ Fiber::Create(cb)                          make_fcontext(栈顶, size, entry)
   │resume()                                        │ 布置寄存器保存区+trampoline
   │ctx_ = jump_fcontext(ctx_, this) ──────────────▶│ 首次进入
   │   （主协程保存区被换出）              entry(transfer_t{主协程恢复点, this})
   │                                          back_ctx_ = from.fctx
   │                                          run(): EXEC -> cb() -> TERM
   │◀────────────────────── jump_fcontext(back_ctx_, this)（结束收尾）
   │ctx_ = 返回值.fctx  ★关键：存回新恢复点
   ▼
子协程内让出（yield_to_hold / yield_to_ready）：
   state = HOLD / READY
   jump_fcontext(back_ctx_, this) ──────────────▶ 回到恢复者 resume() 的返回点
   被 resume 后从这里"返回"，t.fctx 更新 back_ctx_
```
调用resume的是prev协程，所以jump_fcontext的返回代表着回到了prev协程的调用栈。

### 与 ucontext 的关键差异：恢复点要手动保存

ucontext 的 `m_ctx` 会被 swapcontext 自动更新；fcontext 的句柄是一次性的
——**`jump_fcontext` 的返回值 `t.fctx` 才是对方这次被暂停的位置**，恢复方
必须存回 `ctx_`，否则下次 resume 会跳回 make_fcontext 布置的初始入口，
协程函数被从头重跑（真实教训：第一次实现时丢弃返回值，第二次 resume 直接
落到汇编 finish 桩 `_exit(0)`，整个测试进程无提示退出）。

```cpp
// resume()：切进去，回来后记录协程的最新暂停点
ctx_ = detail::jump_fcontext(ctx_, this).fctx;

// yield_to()：跳回恢复者，被再次恢复时更新 back_ctx_（恢复者可能变了）
detail::transfer_t t = detail::jump_fcontext(curr->back_ctx_, curr.get());
curr->back_ctx_ = t.fctx;
```

## 实现细节

### 内存模型：谁保证协程对象活着

- 子协程只经 `Fiber::Create` 工厂创建（shared_ptr 持有），resume 内部用
  `shared_from_this()` 登记，构造函数私有。
- thread_local 两个变量配合：`t_cur`（裸指针，当前协程，零开销访问）+
  `t_cur_holder`（持有引用）。后者保证协程执行期间哪怕外部引用全部消失，
  对象和栈也不会被回收。
- resume() 前保存 `Ptr prev = GetThis()`，切换回来后 `SetThis(prev.get())`
  恢复登记；entry 的 `back_ctx_` 让让出路径无需知道自己被谁恢复过。
- 析构断言状态为 INIT/TERM/EXCEPT：HOLD/READY 的协程被析构意味着将来
  会有 resume 打到已释放的栈（调度器接入后这类错误必须现在就暴露）。

### 回调异常不跨栈传播

协程函数抛出的异常不能跳出 run()（对方的栈帧早已不存在），run() 捕获后
置 EXCEPT 状态并记录错误日志，resume() 正常返回，使用exception_ptr保存在fiber里。

### reset 复用栈

fcontext 的上下文只是栈顶的一份寄存器布局，reset() 对同一块栈重新
make_fcontext 覆盖入口即可整体复用，用于协程池场景（前提：状态为
INIT/TERM/EXCEPT）。

### 配置模块联动

栈大小通过配置模块管理（与 sylar 相同的键名）：

```cpp
auto g_fiber_stack_size = Config::Lookup<uint32_t>(
    "fiber.stack_size", Fiber::kDefaultStackSize, "fiber stack size");
```

YAML 里改 `fiber.stack_size` 后，新建协程即使用新栈大小；构造参数显式
给值时优先于配置。这是配置模块的第一个真实消费者。

### 相对 sylar 的差异汇总

| 项 | sylar | droplet |
| --- | --- | --- |
| 切换原语 | ucontext (getcontext/makecontext/swapcontext) | fcontext（Boost.Context 剥离） |
| 恢复点管理 | swapcontext 自动更新 m_ctx | jump 返回值手动存回 ctx_ |
| 数据传递 | thread_local + GetThis() | transfer_t.data 直接传 this + GetThis() |
| 栈分配 | malloc | aligned_alloc(64)（16 对齐满足 SysV，取 64 更稳） |
| use_caller / CallerMainFunc | 有（配合调度器主函数） | 未实现，留给调度器模块 |
| 日志/锁依赖 | 自研 logger/RWMutex | droplet logger / 标准库 |

## 使用示例

```cpp
#include <droplet/fiber/fiber.h>

int main() {
  auto f = droplet::Fiber::Create([] {
    std::println("in fiber, id={}", droplet::Fiber::GetFiberId());
    droplet::Fiber::yield_to_hold();   // 让出，回到 resume() 的调用点
    std::println("resumed");
  });

  f->resume();   // 输出 "in fiber..."，f 进入 HOLD
  f->resume();   // 从 yield 点继续，输出 "resumed"，f 进入 TERM
}
```

注意约束：协程不能跨线程 resume/yield（TLS 会错乱）；主协程不能 yield；
不要在 HOLD/READY 时析构协程。

## 总结

协程模块 = fcontext 切换原语（两个汇编函数）+ Fiber 封装（状态机、
thread_local 当前协程、指针保全）+ 配置联动的栈大小。切换本身只有几十条
汇编；工程量集中在内存模型（谁在什么时刻持有 Ptr）和 fcontext 特有的
"恢复点手动保存"语义上。测试覆盖主协程、TERM/HOLD/READY/EXCEPT 状态流转、
嵌套 resume、批量交错执行、线程隔离和配置热更新，12 个用例全部通过。

## TODO

- 接入 Scheduler：READY 状态的协程入队、YieldToReady 语义生效。
- use_caller 协程（在主协程上调度的 CallerMainFunc，调度器需要）。
- 栈内存池：reset 复用 + Fiber 池，避免频繁 aligned_alloc/free。
- 支持 valgrind/ASAN 的栈标注（VALGRIND_STACK_REGISTER）。
