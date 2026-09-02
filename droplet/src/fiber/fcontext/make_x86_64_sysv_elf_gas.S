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
#  include "make_i386_sysv_elf_gas.S"
# else
#  if defined __CET__
#   include <cet.h>
#   define SHSTK_ENABLED (__CET__ & 0x2)
#   define BOOST_CONTEXT_SHADOW_STACK (SHSTK_ENABLED && SHADOW_STACK_SYSCALL)
#  else
#   define _CET_ENDBR
#  endif
.file "make_x86_64_sysv_elf_gas.S"
.text
.globl make_fcontext
.hidden make_fcontext
.type make_fcontext,@function
.align 16
make_fcontext:
    /*
     * READING NOTE:
     * C++ 声明：
     *   fcontext_t make_fcontext(void * sp, std::size_t size,
     *                            void (* fn)(transfer_t));
     *
     * x86_64 SysV 参数寄存器：
     *   RDI = sp    新栈栈顶，高地址
     *   RSI = size  栈大小；这个实现主要通过 sp 和后续布局工作，不直接使用 RSI
     *   RDX = fn    第一次进入该 context 时要执行的 C++ 入口函数
     *
     * 返回：
     *   RAX = fcontext_t，也就是指向这块“保存寄存器状态区域”的指针。
     *
     * make_fcontext 不切换上下文，只是在用户提供的新栈上预先摆好
     * jump_fcontext() 将来可以恢复的寄存器/返回地址布局。
     */
    _CET_ENDBR

#if BOOST_CONTEXT_SHADOW_STACK
    /* the new shadow stack pointer (SSP) */
    movq  -0x8(%rdi), %r9
#endif

    /* first arg of make_fcontext() == top of context-stack */
    /* READING NOTE: RAX 后面会成为返回值 fcontext_t。 */
    movq  %rdi, %rax

    /* shift address in RAX to lower 16 byte boundary */
    /* READING NOTE: SysV ABI 要求函数调用边界满足 16 字节对齐。 */
    andq  $-16, %rax

    /* reserve space for context-data on context-stack */
    /* on context-function entry: (RSP -0x8) % 16 == 0 */
    /*
     * READING NOTE:
     * 预留 0x48 字节，形成前面表格里的 fcontext 保存区：
     *   0x00 mxcsr
     *   0x04 x87 cw
     *   0x08 stack guard
     *   0x10 r12
     *   0x18 r13
     *   0x20 r14
     *   0x28 r15
     *   0x30 rbx
     *   0x38 rbp / finish
     *   0x40 rip / trampoline
     */
    leaq  -0x48(%rax), %rax

    /* third arg of make_fcontext() == address of context-function */
    /* stored in RBX */
    /*
     * READING NOTE:
     * 把入口函数 fn 暂存在将来要恢复的 RBX 槽位。
     * 第一次 jump 到该 context 时，jump_fcontext 会恢复 RBX，
     * trampoline 再 jmp *%rbx 进入 C++ 入口函数，比如 fiber_entry/context_entry。
     */
    movq  %rdx, 0x30(%rax)

    /* save MMX control- and status-word */
    stmxcsr  (%rax)
    /* save x87 control-word */
    fnstcw   0x4(%rax)

#if defined(BOOST_CONTEXT_TLS_STACK_PROTECTOR)
    /* save stack guard */
    movq  %fs:0x28, %rcx    /* read stack guard from TLS record */
    movq  %rcx, 0x8(%rax)   /* save stack guard */
#endif

    /* compute abs address of label trampoline */
    leaq  trampoline(%rip), %rcx
    /* save address of trampoline as return-address for context-function */
    /* will be entered after calling jump_fcontext() first time */
    /*
     * READING NOTE:
     * 第一次 jump_fcontext() 恢复这个 context 时，会把 0x40(%rsp)
     * 当成目标 RIP，所以先进入 trampoline，而不是直接进用户 fn。
     */
    movq  %rcx, 0x40(%rax)

    /* compute abs address of label finish */
    leaq  finish(%rip), %rcx
    /* save address of finish as return-address for context-function */
    /* will be entered after context-function returns */
    /*
     * READING NOTE:
     * trampoline 会把这里作为 fn 的返回地址压到栈上。
     * 正常的 Boost.Context 入口函数不会 return 到这里；
     * 如果真的 return，则 finish 调 _exit(0) 兜底终止进程。
     */
    movq  %rcx, 0x38(%rax)

#if BOOST_CONTEXT_SHADOW_STACK
    /* Populate the shadow stack */

    /* get original SSP */
    rdsspq  %r8
    /* restore new shadow stack */
    rstorssp  -0x8(%r9)
    /* save the restore token on the original shadow stack */
    saveprevssp
    /* push the address of "jmp trampoline" to the new shadow stack */
    /* as well as the stack */
    call  1f
    jmp  trampoline
1:
    /* save address of "jmp trampoline" as return-address */
    /* for context-function */
    pop 0x38(%rax)
    /* Get the new SSP.  */
    rdsspq  %r9
    /* restore original shadow stack */
    rstorssp  -0x8(%r8)
    /* save the restore token on the new shadow stack.  */
    saveprevssp

    /* now the new shadow stack looks like:
        base->  +------------------------------+
                | address of "jmp  trampoline" |
        SSP->   +------------------------------+
                |         restore token        |
                +------------------------------+
    */

    /* reserve space for the new SSP */
    leaq  -0x8(%rax), %rax
    /* save the new SSP to this fcontext */
    movq  %r9, (%rax)
#endif

    ret /* return pointer to context-data */

trampoline:
    /*
     * READING NOTE:
     * 第一次恢复新 context 时真正到达这里。
     * 此时 jump_fcontext 已经恢复了 RBX=用户入口函数地址，
     * RDI/RSI 中带着 transfer_t：
     *   RDI = transfer_t.fctx  调用者恢复点
     *   RSI = transfer_t.data  jump_fcontext 第二参数
     *
     * trampoline 的任务：
     *   1. 把 finish 放成入口函数的返回地址；
     *   2. jmp 到 RBX 指向的 C++ 入口函数。
     */
    .cfi_startproc
    .cfi_undefined rip
    /* store return address on stack */
    /* fix stack alignment */
    _CET_ENDBR
#if BOOST_CONTEXT_SHADOW_STACK
    /* save address of "jmp *%rbp" as return-address */
    /* on stack and shadow stack */
    call  2f
    jmp  *%rbp
2:
#else
    /*
     * READING NOTE:
     * RBP 此时来自 0x38(%context-data)，也就是上面保存的 finish。
     * push %rbp 等价于给入口函数布置一个返回地址。
     */
    push %rbp
#endif
    /* jump to context-function */
    /*
     * READING NOTE:
     * 这里不是 call，而是 jmp。入口函数看到的调用现场由上面的
     * 栈布局伪造出来；transfer_t 参数已经在 RDI/RSI。
     */
    jmp *%rbx
    .cfi_endproc

finish:
    /*
     * READING NOTE:
     * 理论兜底路径：如果 context-function 意外普通 return，
     * 就退出整个进程。正常 fiber/continuation 结束会通过
     * ontop_fcontext(..., context_exit/fiber_exit) 回收栈，不会走这里。
     */
    _CET_ENDBR
    /* exit code is zero */
    xorq  %rdi, %rdi
    /* exit application */
    call  _exit@PLT
    hlt
.size make_fcontext,.-make_fcontext

/* Mark that we don't need executable stack. */
.section .note.GNU-stack,"",%progbits
# endif
