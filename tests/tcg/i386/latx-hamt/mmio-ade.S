/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Run inside an x86-64 Linux guest. Normal translated guest addresses are
 * presented through the 0x90 DMW domain, so these aliases exercise the
 * injected-ADE retry path without fabricating a non-canonical guest pointer.
 */
    .section .text
    .globl _start
    .type _start, @function
_start:
    lea buffer(%rip), %r14

    /* base + signed displacement, ld.d */
    movabs $0x1122334455667788, %rax
    mov %rax, 64(%r14)
    mov 64(%r14), %rbx
    movabs $0x1122334455667788, %rcx
    cmp %rcx, %rbx
    jne fail

    /* base + index, with distinct base and destination registers */
    mov $64, %r13
    mov (%r14,%r13,1), %rbx
    cmp %rcx, %rbx
    jne fail

    /* rd == rj: the reloaded destination is also the array base */
    lea buffer+128(%rip), %r12
    mov $16, %r13
    movabs $0x8877665544332211, %rax
    mov %rax, 16(%r12)
    mov (%r12,%r13,1), %r12
    movabs $0x8877665544332211, %rcx
    cmp %rcx, %r12
    jne fail

    /* rd == rk: the reloaded destination is also the array index */
    lea buffer+192(%rip), %r12
    mov $24, %r13
    movabs $0xa5a55a5ac3c33c3c, %rax
    mov %rax, 24(%r12)
    mov (%r12,%r13,1), %r13
    movabs $0xa5a55a5ac3c33c3c, %rcx
    cmp %rcx, %r13
    jne fail

    /* rj == rk: map A and access through A+A at 2*A. */
    mov $9, %rax                    /* mmap */
    movabs $0x200000000000, %rdi
    mov $4096, %rsi
    mov $3, %rdx                    /* PROT_READ | PROT_WRITE */
    mov $0x32, %r10                 /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED */
    mov $-1, %r8
    xor %r9, %r9
    syscall
    movabs $0x200000000000, %r14
    cmp %r14, %rax
    jne fail

    movabs $0x100000000000, %r12
    movabs $0xdeadbeefcafebabe, %rax
    mov %rax, (%r12,%r12,1)
    mov (%r12,%r12,1), %rbx
    movabs $0xdeadbeefcafebabe, %rcx
    cmp %rcx, %rbx
    jne fail

    mov $1, %rax                    /* write */
    mov $1, %rdi
    lea pass(%rip), %rsi
    mov $pass_len, %rdx
    syscall
    xor %rdi, %rdi
    jmp exit

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
    .ascii "PASS: HAMT MMIO ADE guest aliases\n"
    .set pass_len, .-pass
failure:
    .ascii "FAIL: HAMT MMIO ADE guest aliases\n"
    .set failure_len, .-failure

    .section .bss
    .balign 4096
buffer:
    .skip 8192

    .section .note.GNU-stack,"",@progbits
