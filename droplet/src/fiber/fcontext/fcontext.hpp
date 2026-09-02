//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//      (See accompanying file LICENSE_1_0.txt or copy at
//            http://www.boost.org/LICENSE_1_0.txt)
//
// 从 Boost.Context 剥离的 fcontext 接口层（原文件：
// boost/context/detail/fcontext.hpp），去除 boost 宏依赖，
// 并入 droplet::detail 命名空间，仅保留本项目用到的两个原语。
//
// 平台限定：x86_64 / System V ABI / ELF / GNU as，配套的汇编实现见本目录下
// make_x86_64_sysv_elf_gas.S 与 jump_x86_64_sysv_elf_gas.S。
// 其他平台需要补充对应 ABI 变体的汇编文件后再使用本模块。

#pragma once

#include <cstddef>
#include <cstdint>

namespace droplet::detail {

// fcontext_t 是 Boost.Context 的最低层"上下文句柄"，故意暴露成 void*：
// 真正的内容依赖 ABI/汇编实现，通常指向栈上一段保存寄存器状态的内存。
using fcontext_t = void*;

// 每次切换携带的一次性数据：fctx 表示"刚刚被暂停的那个上下文"（即恢复者），
// data 是 jump_fcontext 的第二参数原样传给目标上下文。
struct transfer_t {
  fcontext_t fctx;
  void* data;
};

// 保存当前上下文，恢复 to 指向的上下文，并把 vp 作为 transfer_t::data
// 交给目标上下文。返回时（说明别的上下文又跳回来了），fctx 是对方的恢复点。
extern "C" transfer_t jump_fcontext(fcontext_t const to, void* vp);

// 在栈 [sp - size, sp) 上构造一个"尚未运行"的上下文。
// sp 必须是栈的最高地址（内部会自行做 16 字节对齐）；
// 第一次 jump 到它时，先进入汇编 trampoline，再进入 fn(transfer_t)。
extern "C" fcontext_t make_fcontext(void* sp, std::size_t size,
                                    void (*fn)(transfer_t));

}  // namespace droplet::detail
