/* Synthetic x86 code, no game bytes. Called as (context, observations, restore).
 * Keeps its caller's complete FP state and callee-saved registers. The restore
 * function must jump to .Lresumed, retaining RAX and the other saved registers;
 * returning to the call instruction's continuation traps deliberately.
 *
 * observations: 13 qwords in this order: RAX,RBX,RBP,R12,R13,R14,R15,RDI,
 * RSI,RDX,RCX,R8,R9; qword at0x68 = RSP; at0x70 = saved RSP; at0x78 = RIP.
 * FXSAVE snapshot at0x100, landing marker at0xf0. Buffer must be16-byte aligned.
 */
.text
.p2align 4
.global driveclub_setcontext_test_code
.global driveclub_setcontext_test_end
driveclub_setcontext_test_code:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $0x218,%rsp
    fxsave (%rsp)
    movq %rsi,0x200(%rsp)
    movq %rdi,0x208(%rsp)
    movq %rdx,0x210(%rsp)

    /* Save a real x87 value, SSE vectors and rounding mode in the context. */
    fninit
    fld1
    fxsave 0x120(%rdi)
    movl $0x5f80,0x138(%rdi)
    movabsq $0x0123456789abcdef,%rax
    movq %rax,0x1c0(%rdi)
    movabsq $0xfedcba9876543210,%rax
    movq %rax,0x1c8(%rdi)
    movabsq $0x1020304050607080,%rax
    movq %rax,0x230(%rdi)
    movabsq $0x8070605040302010,%rax
    movq %rax,0x238(%rdi)
    movq %rsp,0xf8(%rdi)
    leaq .Lresumed(%rip),%rax
    movq %rax,0xe0(%rdi)

    /* The state to be restored differs from the state at the import call. */
    fninit
    pxor %xmm0,%xmm0
    pxor %xmm7,%xmm7
    callq *0x210(%rsp)
    ud2
.Lresumed:
    movq 0x200(%rsp),%r10
    movq %rax,0x00(%r10)
    movq %rbx,0x08(%r10)
    movq %rbp,0x10(%r10)
    movq %r12,0x18(%r10)
    movq %r13,0x20(%r10)
    movq %r14,0x28(%r10)
    movq %r15,0x30(%r10)
    movq %rdi,0x38(%r10)
    movq %rsi,0x40(%r10)
    movq %rdx,0x48(%r10)
    movq %rcx,0x50(%r10)
    movq %r8,0x58(%r10)
    movq %r9,0x60(%r10)
    movq %rsp,0x68(%r10)
    movq 0x208(%rsp),%r11
    movq 0xf8(%r11),%rax
    movq %rax,0x70(%r10)
    leaq .Lresumed(%rip),%rax
    movq %rax,0x78(%r10)
    fxsave 0x100(%r10)
    movq $1,0xf0(%r10)

    fxrstor (%rsp)
    addq $0x218,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    movl $42,%eax
    retq
driveclub_setcontext_test_end:
.section .note.GNU-stack,"",@progbits
