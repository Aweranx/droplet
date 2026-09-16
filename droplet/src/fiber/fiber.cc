#include "droplet/fiber/fiber.h"
#include <droplet/types.h>

#include "fcontext/fcontext.hpp"

#include <droplet/config/config.h>
#include <droplet/logger/log.h>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <new>
#include <utility>

namespace droplet {

namespace {

std::atomic<u64> s_fiber_id{0};
std::atomic<u64> s_fiber_count{0};

// 线程的"当前协程"：t_cur 提供无开销访问；t_cur_holder 维持其存活，
// 保证协程执行期间哪怕外部引用全部消失，栈和对象也不会被回收。
thread_local Fiber* t_cur = nullptr;
thread_local Fiber::Ptr t_cur_holder = nullptr;

// 栈对齐：SysV ABI 要求 16 字节，取 64 覆盖更多平台的栈对齐要求。
constexpr u32 kStackAlign = 64;

// 协程栈大小配置项：与 sylar 相同的键名，可在 YAML 里通过
// fiber.stack_size 热更新，之后新建的协程即使用新栈大小。
auto g_fiber_stack_size =
    Config::Lookup<u32>("fiber.stack_size", Fiber::kDefaultStackSize,
                             "fiber stack size");

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Fiber::Fiber() {
  // 主协程代表线程自身的调用栈：无需分配栈、无需布置上下文，
  // 永远处于 EXEC；随 thread_local 在线程退出时销毁。
  state_ = State::EXEC;
  ++s_fiber_count;
  DROPLET_LOG_DEBUG(GetRootLogger()) << "Fiber 主协程创建";
}

Fiber::Fiber(std::function<void()> cb, u32 stack_size)
    : id_(++s_fiber_id), cb_(std::move(cb)) {
  ++s_fiber_count;

  u32 size = stack_size ? stack_size
                             : (g_fiber_stack_size ? g_fiber_stack_size->getValue()
                                                   : kDefaultStackSize);
  if (size < kMinStackSize) {
    size = kMinStackSize;
  }
  // aligned_alloc 要求 size 是对齐值的整数倍，这里向上取整。
  stack_size_ = (size + kStackAlign - 1) & ~(kStackAlign - 1);
  stack_ = std::aligned_alloc(kStackAlign, stack_size_);
  if (!stack_) {
    throw std::bad_alloc();
  }

  // make_fcontext 接收栈的最高地址，内部自行做 16 字节对齐并布置
  // 寄存器保存区 + trampoline；首次 jump 时进入 Fiber::entry。
  ctx_ = detail::make_fcontext(static_cast<char*>(stack_) + stack_size_,
                               stack_size_, &Fiber::entry);
  state_ = State::INIT;
  DROPLET_LOG_DEBUG(GetRootLogger())
      << "Fiber 创建 id=" << id_ << " stack=" << stack_size_;
}

Fiber::~Fiber() {
  --s_fiber_count;
  if (stack_) {
    // 只有已经离开协程栈才允许析构；HOLD/READY 析构会让潜在的
    // resume 变成悬垂访问（调度器接入后这类错误必须暴露）。
    DROPLET_ASSERT(state_ == State::TERM || state_ == State::EXCEPT ||
                   state_ == State::INIT);
    std::free(stack_);
  } else if (t_cur == this) {
    // 主协程：随 thread_local 在线程退出时销毁，这里只清理指针。
    t_cur = nullptr;
  }
}

// ---------------------------------------------------------------------------
// 创建 / 重置
// ---------------------------------------------------------------------------

Fiber::Ptr Fiber::Create(std::function<void()> cb, u32 stack_size) {
  // 强制经由 shared_ptr 持有：resume/entry 依赖 shared_from_this。
  return Ptr(new Fiber(std::move(cb), stack_size));
}

void Fiber::reset(std::function<void()> cb) {
  DROPLET_ASSERT(stack_);
  DROPLET_ASSERT(state_ == State::INIT || state_ == State::TERM ||
                 state_ == State::EXCEPT);
  cb_ = std::move(cb);
  exception_ = nullptr;
  // fcontext 的上下文只是栈顶的一份寄存器布局：重新 make_fcontext
  // 覆盖入口即可整体复用旧栈，代价远小于重新分配。
  ctx_ = detail::make_fcontext(static_cast<char*>(stack_) + stack_size_,
                               stack_size_, &Fiber::entry);
  state_ = State::INIT;
}

// ---------------------------------------------------------------------------
// 切换
// ---------------------------------------------------------------------------

void Fiber::resume() {
  // 先登记恢复者：GetThis() 同时确保线程主协程已创建。
  Ptr prev = GetThis();
  DROPLET_ASSERT(prev.get() != this && "不能 resume 当前正在执行的协程");
  DROPLET_ASSERT(state_ != State::EXEC && "协程正在执行中");
  DROPLET_ASSERT(state_ != State::TERM && state_ != State::EXCEPT &&
                 "已结束的协程不能 resume");

  SetThis(this);
  state_ = State::EXEC;
  // data 传 this：entry 首次进入时据此取回 Fiber*（fcontext 没有类似
  // makecontext 的闭包参数，指针跟着切换传过去）。
  //
  // fcontext 与 ucontext 的关键差异：m_ctx 不会被自动更新，本次切换后
  // 协程的新暂停点在"返回值"里（t.fctx 指向协程栈上最新保存的寄存器
  // 区域），必须存回 ctx_ 作为下一次 resume 的恢复点，否则会重新进入
  // make_fcontext 布置的初始入口。
  ctx_ = detail::jump_fcontext(ctx_, this).fctx;
  // 回到这里说明协程 yield 或结束，恢复线程的"当前协程"登记。
  SetThis(prev.get());
}

void Fiber::yield_to_hold() { yield_to(State::HOLD); }

void Fiber::yield_to_ready() { yield_to(State::READY); }

void Fiber::yield_to(State state) {
  Ptr curr = GetThis();
  DROPLET_ASSERT(curr->stack_ && "主协程没有独立栈，不能 yield");
  DROPLET_ASSERT(curr->state_ == State::EXEC);

  curr->state_ = state;
  // 被再次 resume 时，这次 jump 会"返回"，此时恢复者可能已变化，
  // 因此要用返回的 transfer 更新 back_ctx_。
  detail::transfer_t t = detail::jump_fcontext(curr->back_ctx_, curr.get());
  curr->back_ctx_ = t.fctx;
}

void Fiber::SetThis(Fiber* f) {
  // 通过 shared_from_this 登记，协程执行期间线程自己持有一份引用；
  // 主协程同样成立（其所有权最终在 thread_local 上）。
  t_cur_holder = f ? f->shared_from_this() : nullptr;
  t_cur = f;
}

Fiber::Ptr Fiber::GetThis() {
  if (!t_cur) {
    // 每线程首个协程：主协程，代表线程自身的栈，生命周期到线程退出。
    Ptr main(new Fiber());
    t_cur = main.get();
    t_cur_holder = std::move(main);
  }
  return t_cur_holder;
}

u64 Fiber::GetFiberId() { return t_cur ? t_cur->id_ : 0; }

u64 Fiber::TotalFibers() { return s_fiber_count.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// 协程体执行
// ---------------------------------------------------------------------------

void Fiber::entry(detail::transfer_t from) {
  auto* self = static_cast<Fiber*>(from.data);
  // 记住恢复者的上下文：yield 与结束收尾都跳回它。
  self->back_ctx_ = from.fctx;
  self->run();
  // run() 末尾已跳回恢复者，不会到达这里；若真到达，
  // make_fcontext 的 finish 桩会以 _exit 兜底终止进程。
}

void Fiber::run() {
  state_ = State::EXEC;
  try {
    cb_();
  } catch (const std::exception& e) {
    // 异常不能跨协程栈传播（对方栈帧早已不同），转成 EXCEPT 状态上报。
    exception_ = std::current_exception();
    state_ = State::EXCEPT;
    DROPLET_LOG_ERROR(GetRootLogger())
        << "Fiber 未捕获异常 what=" << e.what();
  } catch (...) {
    exception_ = std::current_exception();
    state_ = State::EXCEPT;
    DROPLET_LOG_ERROR(GetRootLogger())
        << "Fiber 未捕获未知异常";
  }
  cb_ = nullptr;
  if (state_ == State::EXEC) {
    state_ = State::TERM;
  }
  // 跳回恢复者：此时 t_cur_holder 仍持有本协程，跨过这次切换、
  // 恢复者执行 SetThis(prev) 之后，本协程的引用才交还给外部。
  (void)detail::jump_fcontext(back_ctx_, this);
  // 不可达；abort 而不是返回，避免落入汇编的 finish->_exit 掩盖错误。
  std::abort();
}

}  // namespace droplet
