/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)
*/

/****************************************************************************************
 *                                                                                      *
 *  ----------------------------------------------------------------------------------  *
 *  |    0    |    1    |    2    |    3    |    4     |    5    |    6    |    7    |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x0   |   0x4   |   0x8   |   0xc   |   0x10   |   0x14  |   0x18  |   0x1c  |  *
 *  ----------------------------------------------------------------------------------  *
 *  | fc_mxcsr|fc_x87_cw|       guard       |         R12        |        R13        |  *
 *  ----------------------------------------------------------------------------------  *
 *  ----------------------------------------------------------------------------------  *
 *  |    8    |    9    |   10    |   11    |    12    |    13   |    14   |    15   |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x20  |   0x24  |   0x28  |  0x2c   |   0x30   |   0x34  |   0x38  |   0x3c  |  *
 *  ----------------------------------------------------------------------------------  *
 *  |        R14        |        R15        |         RBX        |        RBP        |  *
 *  ----------------------------------------------------------------------------------  *
 *  ----------------------------------------------------------------------------------  *
 *  |   16    |   17    |   18    |   19    |    20    |    21   |    22   |    23   |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x40  |   0x44  |                                                            |  *
 *  ----------------------------------------------------------------------------------  *
 *  |        RIP        |                                                            |  *
 *  ----------------------------------------------------------------------------------  *
 *                                                                                      *
 ****************************************************************************************/

# ifdef __i386__
#  include "jump_i386_sysv_elf_gas.S"
# else
#  if defined __CET__
#   include <cet.h>
#   define SHSTK_ENABLED (__CET__ & 0x2)
#   define BOOST_CONTEXT_SHADOW_STACK (SHSTK_ENABLED && SHADOW_STACK_SYSCALL)
#  else
#   define _CET_ENDBR
#  endif
.file "jump_x86_64_sysv_elf_gas.S"
.text
.globl jump_fcontext
.hidden jump_fcontext
.type jump_fcontext,@function
.align 16
jump_fcontext:
    /*
     * READING NOTE:
     * C++ 声明：
     *   transfer_t jump_fcontext(fcontext_t const to, void * vp);
     *
     * x86_64 SysV 参数寄存器：
     *   RDI = to  目标 context 的保存区指针
     *   RSI = vp  本次要传给目标 context 的 data
     *
     * 语义：
     *   1. 在当前栈上保存当前 context；
     *   2. 把当前 context 的保存区地址放到 RAX；
     *   3. 切到 RDI 指向的目标 context；
     *   4. 恢复目标寄存器和目标栈；
     *   5. 跳到目标保存的 RIP。
     *
     * 对目标而言，本次切换收到的 transfer_t 是：
     *   fctx = RAX = 当前 context 的恢复点
     *   data = RSI = 调用 jump_fcontext 时传入的 vp
     */
    _CET_ENDBR
    leaq  -0x40(%rsp), %rsp /* prepare stack */

#if !defined(BOOST_USE_TSX)
    /*
     * READING NOTE:
     * 保存浮点/SIMD 控制状态。这里不是保存所有 XMM 数据寄存器；
     * 按 SysV ABI，XMM 寄存器是 caller-saved。
     */
    stmxcsr  (%rsp)     /* save MMX control- and status-word */
    fnstcw   0x4(%rsp)  /* save x87 control-word */
#endif

#if defined(BOOST_CONTEXT_TLS_STACK_PROTECTOR)
    movq  %fs:0x28, %rcx    /* read stack guard from TLS record */
    movq  %rcx, 0x8(%rsp)   /* save stack guard */
#endif

    movq  %r12, 0x10(%rsp)  /* save R12 */
    movq  %r13, 0x18(%rsp)  /* save R13 */
    movq  %r14, 0x20(%rsp)  /* save R14 */
    movq  %r15, 0x28(%rsp)  /* save R15 */
    movq  %rbx, 0x30(%rsp)  /* save RBX */
    movq  %rbp, 0x38(%rsp)  /* save RBP */
    /*
     * READING NOTE:
     * 保存 SysV ABI 的 callee-saved 通用寄存器：
     *   RBX, RBP, R12-R15。
     * 当前函数的返回地址原本在 0x40(%rsp)，不需要显式 mov 保存。
     */

#if BOOST_CONTEXT_SHADOW_STACK
    /* grow the stack to reserve space for shadow stack pointer(SSP) */
    leaq  -0x8(%rsp), %rsp
    /* read the current SSP and store it */
    rdsspq  %rcx
    movq  %rcx, (%rsp)
# endif

    /* store RSP (pointing to context-data) in RAX */
    /*
     * READING NOTE:
     * RAX 现在就是“当前 context 的 fcontext_t”。
     * 稍后它会作为 transfer_t.fctx 交给目标 context。
     */
    movq  %rsp, %rax

    /* restore RSP (pointing to context-data) from RDI */
    /*
     * READING NOTE:
     * 真正的栈切换发生在这里：
     *   RSP 从当前栈保存区切换为目标 context 保存区。
     */
    movq  %rdi, %rsp

#if BOOST_CONTEXT_SHADOW_STACK
    /* first 8 bytes are SSP */
    movq  (%rsp), %rcx
    leaq  0x8(%rsp), %rsp

    /* Restore target(new) shadow stack */
    rstorssp  -8(%rcx)
    /* restore token for previous shadow stack is pushed */
    /* on previous shadow stack after saveprevssp */
    saveprevssp

    /* when return, jump_fcontext jump to restored return address */
    /* (r8) instead of RET. This miss of RET implies us to unwind */
    /* shadow stack accordingly. Otherwise mismatch occur */
    movq  $1, %rcx
    incsspq  %rcx
#endif

    movq  0x40(%rsp), %r8  /* restore return-address */
    /*
     * READING NOTE:
     * 目标 context 的保存区里，0x40 保存目标 RIP。
     * 初次进入 make_fcontext 创建的 context 时，这里是 trampoline；
     * 后续普通 yield/resume 时，这里是某次 jump_fcontext 调用点之后。
     */

#if !defined(BOOST_USE_TSX)
    /* READING NOTE: 恢复目标 context 的浮点控制状态。 */
    ldmxcsr  (%rsp)     /* restore MMX control- and status-word */
    fldcw    0x4(%rsp)  /* restore x87 control-word */
#endif

#if defined(BOOST_CONTEXT_TLS_STACK_PROTECTOR)
    movq  0x8(%rsp), %rdx  /* load stack guard */
    movq  %rdx, %fs:0x28   /* restore stack guard to TLS record */
#endif

    movq  0x10(%rsp), %r12  /* restore R12 */
    movq  0x18(%rsp), %r13  /* restore R13 */
    movq  0x20(%rsp), %r14  /* restore R14 */
    movq  0x28(%rsp), %r15  /* restore R15 */
    movq  0x30(%rsp), %rbx  /* restore RBX */
    movq  0x38(%rsp), %rbp  /* restore RBP */
    /* READING NOTE: 恢复目标 context 的 callee-saved 通用寄存器。 */

    leaq  0x48(%rsp), %rsp /* prepare stack */
    /*
     * READING NOTE:
     * 跳过目标 context 保存区和目标 RIP 槽位。
     * 之后 RSP 恢复为目标函数继续执行时应看到的栈顶。
     */

    /* return transfer_t from jump */
#if !defined(_ILP32)
    /* RAX == fctx, RDX == data */
    /*
     * READING NOTE:
     * x86_64 SysV 下，两个指针大小的结构体 transfer_t
     * 可以用 RAX/RDX 返回：
     *   RAX = transfer_t.fctx = 刚刚暂停的当前 context
     *   RDX = transfer_t.data = vp
     */
    movq  %rsi, %rdx
#else
    /* RAX == data:fctx */
    salq  $32, %rsi
    orq   %rsi, %rax
#endif
    /* pass transfer_t as first arg in context function */
#if !defined(_ILP32)
    /* RDI == fctx, RSI == data */
#else
    /* RDI == data:fctx */
#endif
    /*
     * READING NOTE:
     * 如果目标 RIP 是 trampoline -> C++ 入口函数，
     * 那么入口函数参数 transfer_t 需要放在 RDI/RSI。
     * 如果目标只是从 jump_fcontext 返回，RAX/RDX 作为返回值已经足够；
     * 设置 RDI 不影响这种情况。
     */
    movq  %rax, %rdi

    /* indirect jump to context */
    /*
     * READING NOTE:
     * 不用 ret，而是 jmp 到保存的 RIP。
     * 从 C++ 视角看，目标 context 中之前那次 jump_fcontext()
     * 调用“返回”了一个 transfer_t。
     */
    jmp  *%r8
.size jump_fcontext,.-jump_fcontext

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits
# endif
