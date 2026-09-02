#pragma once

#include <droplet/export.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace droplet {

class Scheduler;

namespace detail {
struct transfer_t;  // boost fcontext 的切换结构，见 src/fiber/fcontext/fcontext.hpp
using fcontext_t = void*;
}  // namespace detail

/**
 * @brief 有栈协程。
 * @details 底层使用从 Boost.Context 剥离的 fcontext 汇编（make_fcontext /
 *          jump_fcontext）做上下文切换：只保存/恢复 callee-saved 寄存器并换栈，
 *          不陷入内核，单次切换约几十纳秒，比 ucontext（每次切换保存信号掩码，
 *          需要系统调用）快一个数量级。
 *
 *          使用约束（有栈协程的通例，违反即为未定义行为）：
 *          - 协程必须在其创建线程上 resume/yield，不允许跨线程切换，
 *            否则 TLS 状态（logger、当前协程指针等）会错乱；
 *          - 处于 HOLD/READY 的协程不允许析构：持有 Ptr 的一方负责协程存活，
 *            后续调度器会依赖存活协程；
 *          - 协程内部不能 resume 自己；主协程（代表线程自身栈）不能被 resume。
 *
 *          状态流转：
 *          INIT --resume--> EXEC --回调结束--> TERM
 *           ^                |
 *           |           yield_to_hold / yield_to_ready
 *           |                v
 *           +---reset---- HOLD / READY --resume--> EXEC
 *          回调抛出未捕获异常时进入 EXCEPT（异常不跨栈传播，仅记录日志）。
 */
class DROPLET_API Fiber final : public std::enable_shared_from_this<Fiber> {
 public:
  enum class State : uint8_t {
    INIT,    ///< 已创建/已重置，尚未开始运行
    HOLD,    ///< 让出后暂停，等待 resume
    EXEC,    ///< 正在执行
    TERM,    ///< 执行完毕
    READY,   ///< 让出时主动声明可立即再次运行（供后续调度器使用）
    EXCEPT,  ///< 执行中抛出未捕获异常
  };
  using Ptr = std::shared_ptr<Fiber>;

  /// 默认栈大小（字节），与 sylar 一致；为 0 时读取配置项 fiber.stack_size
  static constexpr uint32_t kDefaultStackSize = 128 * 1024;
  /// 栈大小下限：fcontext 布局 + 入口函数帧至少需要几 KB
  static constexpr uint32_t kMinStackSize = 16 * 1024;

  /// 创建协程；stack_size 为 0 时使用配置项 fiber.stack_size 的当前值。
  [[nodiscard]] static Ptr Create(std::function<void()> cb,
                                  uint32_t stack_size = 0);

  /// 析构会释放协程栈；要求协程已离开自己的栈（见类注释的存活约束）。
  ~Fiber();

  /**
   * @brief 重置执行函数并复用已分配的栈。
   * @pre getState() 为 INIT / TERM / EXCEPT
   * @post getState() == INIT，再次 resume 时从头执行新函数
   */
  void reset(std::function<void()> cb);

  /**
   * @brief 从当前上下文切换进该协程。
   * @pre 当前不在该协程上，且状态为 INIT / HOLD / READY
   * @post 该协程进入 EXEC；本次调用在协程让出或结束时返回
   */
  void resume();

  /// 当前协程让出并回到恢复者，状态置 HOLD；仅能在协程内部调用。
  static void yield_to_hold();
  /// 同 yield_to_hold，但状态置 READY，供调度器重新入队。
  static void yield_to_ready();

  /// 当前线程所在的协程；首次调用会为线程创建主协程。
  [[nodiscard]] static Ptr GetThis();
  /// 当前协程 id；尚未进入协程环境（thread_local 未初始化）时返回 0。
  [[nodiscard]] static uint64_t GetFiberId();
  /// 存活协程总数（含各线程的主协程）。
  [[nodiscard]] static uint64_t TotalFibers();

  [[nodiscard]] uint64_t getId() const noexcept { return id_; }
  [[nodiscard]] State getState() const noexcept { return state_; }
  /// 实际栈大小（字节）；主协程为 0。
  [[nodiscard]] uint32_t getStackSize() const noexcept { return stack_size_; }

 private:
  friend class Scheduler;  // 后续调度器需要直接操作上下文与状态

  /// 主协程：代表线程自身的调用栈，无独立栈，每线程仅一份。
  Fiber();
  /// 子协程：分配独立栈并布置 fcontext 入口。
  Fiber(std::function<void()> cb, uint32_t stack_size);

  /// 登记线程的"当前协程"（同时维护裸指针与持有引用）。
  static void SetThis(Fiber* f);
  /// 让出公共实现：置状态并跳回恢复者。
  static void yield_to(State state);
  /// fcontext 入口跳板：首次 resume 进入，登记 back_ctx_ 后执行 run()。
  static void entry(detail::transfer_t from);
  /// 执行回调并收尾（TERM/EXCEPT），最后跳回恢复者，不再返回。
  void run();

  uint64_t id_ = 0;
  uint32_t stack_size_ = 0;
  void* stack_ = nullptr;  // 主协程为 nullptr（直接用线程自身栈）
  std::function<void()> cb_;
  State state_ = State::INIT;
  detail::fcontext_t ctx_ = nullptr;       // 本协程上下文（jump 的目标）
  detail::fcontext_t back_ctx_ = nullptr;  // 最近一次进入时的恢复者上下文
};

}  // namespace droplet
