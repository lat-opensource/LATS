/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Exercise translated VLD/VST and, when AVX is available, XVLD/XVST through
 * ordinary x86 vector memory operands.
 */
    .section .text
    .globl _start
    .type _start, @function
_start:
    lea source(%rip), %r12
    lea destination(%rip), %r13

    /* SSE 128-bit load/store, normally translated as VLD/VST. */
    movdqu (%r12), %xmm0
    movdqu %xmm0, (%r13)
    movq 8(%r13), %rax
    movabs $0x8877665544332211, %rcx
    cmp %rcx, %rax
    jne fail

    /* AVX 256-bit load/store, for translators using XVLD/XVST. */
    mov $1, %eax
    cpuid
    bt $28, %ecx                    /* AVX */
    jnc sse_ok

    lea source+4096(%rip), %r12
    lea destination+4096(%rip), %r13
    vmovdqu (%r12), %ymm0
    vmovdqu %ymm0, (%r13)
    movq 24(%r13), %rax
    movabs $0x0123456789abcdef, %rcx
    cmp %rcx, %rax
    jne fail_avx
    vzeroupper

sse_ok:
    mov $1, %rax                    /* write */
    mov $1, %rdi
    lea pass(%rip), %rsi
    mov $pass_len, %rdx
    syscall
    xor %rdi, %rdi
    jmp exit

fail_avx:
    vzeroupper
fail:
    mov $1, %rax                    /* write */
    mov $1, %rdi
    lea failure(%rip), %rsi
    mov $failure_len, %rdx
    syscall
    mov $1, %rdi

exit:
    mov $60, %rax                   /* exit */
    syscall
    .size _start, .-_start

    .section .rodata
pass:
    .ascii "PASS: HAMT vector MMIO ADE\n"
    .set pass_len, .-pass
failure:
    .ascii "FAIL: HAMT vector MMIO ADE\n"
    .set failure_len, .-failure

    .section .data
    .balign 32
source:
    .quad 0x1122334455667788, 0x8877665544332211
    .quad 0x0123456789abcdef, 0xfedcba9876543210
    .skip 4096 - 32
    .quad 0x0f1e2d3c4b5a6978, 0x1020304050607080
    .quad 0x90a0b0c0d0e0f000, 0x0123456789abcdef
    .skip 4096 - 32

    .section .bss
    .balign 32
destination:
    .skip 8192

    .section .note.GNU-stack,"",@progbits
