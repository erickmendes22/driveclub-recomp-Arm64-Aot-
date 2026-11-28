/* Register restore sequence adapted from libunwind, src/x86_64/setcontext.S.
 * Copyright (C) 2007 Google, Inc. Contributed by Arun Sharma.
 * Copyright (C) 2010 Konstantin Belousov.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Orbis HP-libunwind resume entry, executed as x86-64 guest code by FEX and
 * directly on x86-64 hosts. No host ucontext_t is involved.
 *
 * Layout verified against the supplied CUSA00093 libc.prx getcontext at
 * 0x53cd0: machine context starts at 0x40, mc_len=0x480, FXSAVE at 0x120.
 * Register/FP restore semantics follow libunwind's FreeBSD setcontext.S:
 * https://github.com/libunwind/libunwind/blob/master/src/x86_64/setcontext.S
 * R10/R11 are not saved by that producer. Signal masks, segment bases and
 * RFLAGS are not restored by this libunwind resume operation; sigreturn is a
 * separate API. The FEX/host TLS bases must not be changed to Bionic values.
 *
 * tools/generate-setcontext.py generates the architecture-neutral byte array
 * consumed by both CPU bridges; the failure target is relocated at runtime.
 */
.text
.p2align 4
.global driveclub_setcontext_code
.global driveclub_setcontext_failure_target
.global driveclub_setcontext_end
driveclub_setcontext_code:
    testq %rdi,%rdi
    jz .Lbad_pointer
    testq $15,%rdi
    jnz .Lbad_pointer
    cmpq $0x480,0x108(%rdi)
    jne .Lbad_length
    cmpq $0,0xe0(%rdi)
    je .Lbad_control
    cmpq $16,0xf8(%rdi)
    jb .Lbad_control
    cmpq $0x20000,0x118(%rdi)
    je .Lregisters
    cmpq $0x20002,0x118(%rdi)
    je .Lregisters
    cmpq $0x20001,0x118(%rdi)
    jne .Lbad_fp_owner
    cmpq $0x10002,0x110(%rdi)
    jne .Lbad_fp_format
    fxrstor 0x120(%rdi)
.Lregisters:
    movq 0x68(%rdi),%r8
    movq 0x70(%rdi),%r9
    movq 0x80(%rdi),%rbx
    movq 0x88(%rdi),%rbp
    movq 0xa0(%rdi),%r12
    movq 0xa8(%rdi),%r13
    movq 0xb0(%rdi),%r14
    movq 0xb8(%rdi),%r15
    movq 0x50(%rdi),%rsi
    movq 0x58(%rdi),%rdx
    /* Read all context fields before switching stacks: the context itself may
     * be on the stack being abandoned. Preserve restored RAX (exception data). */
    movq 0xf8(%rdi),%rcx
    movq 0xe0(%rdi),%rax
    movq %rax,-8(%rcx)
    movq 0x48(%rdi),%rax
    movq %rax,-16(%rcx)
    movq 0x78(%rdi),%rax
    movq 0x60(%rdi),%rcx
    movq 0xf8(%rdi),%rsp
    leaq -16(%rsp),%rsp
    popq %rdi
    retq
.Lbad_pointer:
    movl $1,%esi
    jmp .Linvalid
.Lbad_length:
    movl $2,%esi
    jmp .Linvalid
.Lbad_control:
    movl $3,%esi
    jmp .Linvalid
.Lbad_fp_owner:
    movl $4,%esi
    jmp .Linvalid
.Lbad_fp_format:
    movl $5,%esi
.Linvalid:
    /* Absolute jump to a normal import thunk on FEX, SysV function on x86. */
    .byte 0x48,0xb8
driveclub_setcontext_failure_target:
    .quad 0
    jmp *%rax
driveclub_setcontext_end:
.section .note.GNU-stack,"",@progbits
