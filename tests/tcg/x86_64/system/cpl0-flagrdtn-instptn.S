/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPL0 flag reduction (flagrdtn) and instruction pattern (instptn)
 * regression test. Exercises arithmetic/logic/shift/BT/CMP/TEST/SSE
 * patterns, dead/live flag preservation, helper-calling MOV/string MOVSD
 * bridges, CR0.TS #NM delivery on fused SSE compares, and #DB single-step.
 */
.text
.global main
main:
    /* Enable SSE (CR0.MP=1, CR0.EM=0, CR0.TS=0, CR4.OSFXSR=1). */
    mov %cr0, %rax
    and $~0xc, %rax
    or $0x2, %rax
    mov %rax, %cr0
    mov %cr4, %rax
    or $0x200, %rax
    mov %rax, %cr4

    /* Install #DB (vec 1) and #NM (vec 7) handlers in IDT. */
    leaq debug_handler(%rip), %rax
    movw %ax, idt+16(%rip)
    movw %cs, idt+18(%rip)
    movw $0x8e00, idt+20(%rip)
    shrq $16, %rax
    movw %ax, idt+22(%rip)

    leaq nm_handler(%rip), %rax
    movw %ax, idt+112(%rip)
    movw %cs, idt+114(%rip)
    movw $0x8e00, idt+116(%rip)
    shrq $16, %rax
    movw %ax, idt+118(%rip)

    lidt idtr(%rip)

    /* 1. CMP/TEST + Jcc across 8/16/32/64-bit, reg/imm/mem, and flag preservation. */
    movb $'1', %r15b
    movabsq $0x100000000, %rax
    testq %rax, %rax
    je fail
    cmpq $0, %rax
    je fail
    movq $-1, %rbx
    cmpq %rbx, %rax
    jne 1f
    jmp fail
1:  jnc fail

    movq %rax, %rbx
    cmpq %rbx, %rax
    jne fail
    jc fail

    movb $0x80, %al
    cmpb $0x7f, %al
    jbe fail
    cmpb $0x7f, %al
    jg fail

    movw $0x1234, %ax
    cmpw $0x1234, %ax
    jne fail

    movl $5, mem_val(%rip)
    cmpl $5, mem_val(%rip)
    jne fail
    testl $4, mem_val(%rip)
    je fail

    /* 2. Direct jump into Jcc of a fused TEST+Jcc pair. */
    movb $'2', %r15b
    xorl %eax, %eax
    movq $1, %r8
    testq %rax, %rax
2:  jne 3f
    cmpq $1, %r8
    jne fail
    movl $0, %r8d
    orq $1, %rbx
    jmp 2b
3:  testq %r8, %r8
    jne fail

    /* 3. Dead-flag reduction and partial-flag preservation (CF across INC/DEC, CL=0 shift). */
    movb $'3', %r15b
    movq $7, %r8
    movq $4, %r9
    addq %r8, %r9
    cmpq %r8, %r9
    testq %r8, %r8
    xorq %rax, %rax
    jne fail

    stc
    movl $1, %eax
    incq %rax
    jnc fail
    stc
    decq %rax
    jnc fail

    xorl %ecx, %ecx
    stc
    movq $0x1234, %rax
    shlq %cl, %rax
    jnc fail

    /* 4. ALU + Jcc and XX_JCC bridges (ADD/SUB/AND/OR/XOR/DEC/SAR/SHR/BT). */
    movb $'4', %r15b
    movq $10, %rax
    subq $10, %rax
    jne fail
    movq $3, %rax
    addq $-3, %rax
    jne fail
    movq $0xf0, %rax
    andq $0x0f, %rax
    jne fail
    movq $0, %rax
    orq $0, %rax
    jne fail
    movq $0x55, %rax
    xorq $0x55, %rax
    jne fail
    movq $1, %rax
    decq %rax
    jne fail
    movq $8, %rax
    shrq $3, %rax
    je fail
    movq $-16, %rax
    sarq $2, %rax
    jns fail

    movq $0x20, %rax
    btq $5, %rax
    jae fail
    btq $4, %rax
    jb fail

    /* XX_JCC with ordinary MOV/LEA/PUSH bridge. */
    movq $42, %rax
    cmpq $42, %rax
    movq $99, %r8
    leaq 8(%r8), %r9
    pushq %r9
    jne fail
    popq %r9
    cmpq $107, %r9
    jne fail

    /* 5. CMP + helper-calling MOV (CR0) + Jcc must NOT clobber condition. */
    movb $'5', %r15b
    movq $11, %rax
    movq $22, %rbx
    cmpq %rbx, %rax
    mov %cr0, %rcx
    jge fail
    cmpq %rax, %rbx
    mov %cr0, %rcx
    jle fail

    /* 6. CMP + string MOVSD (0xa5) + Jcc must NOT clobber condition. */
    movb $'6', %r15b
    cld
    movl $0x12345678, str_src(%rip)
    movl $0, str_dst(%rip)
    leaq str_src(%rip), %rsi
    leaq str_dst(%rip), %rdi
    movq $1, %rax
    movq $2, %rbx
    cmpq %rbx, %rax
    movsl
    jge fail
    cmpl $0x12345678, str_dst(%rip)
    jne fail

    /* 7. CMP+SETcc/CMOVcc, TEST+SETcc/CMOVcc, CMP+SBB, CQO+IDIV, CDQ+IDIV. */
    movb $'7', %r15b
    movq $5, %rax
    movq $9, %rbx
    xorl %ecx, %ecx
    cmpq %rbx, %rax
    setb %cl
    cmovbq %rbx, %rax
    cmpb $1, %cl
    jne fail
    cmpq $9, %rax
    jne fail

    movq $5, %rax
    movq $9, %rbx
    cmpq %rbx, %rax
    sbbq %rdx, %rdx
    cmpq $-1, %rdx
    jne fail

    movq $-100, %rax
    movq $7, %rcx
    cqo
    idivq %rcx
    cmpq $-14, %rax
    jne fail
    cmpq $-2, %rdx
    jne fail

    movl $-100, %eax
    movl $7, %ecx
    cdq
    idivl %ecx
    cmpl $-14, %eax
    jne fail
    cmpl $-2, %edx
    jne fail

    /* 8. UCOMISD + Jcc with CR0.TS=0, then #NM delivery with CR0.TS=1. */
    movb $'8', %r15b
    movsd fp_one(%rip), %xmm0
    movsd fp_two(%rip), %xmm1
    ucomisd %xmm1, %xmm0
    jae fail
    ucomisd %xmm0, %xmm1
    jbe fail

    xorl %r12d, %r12d
    mov %cr0, %rax
    or $0x8, %rax                /* Set CR0.TS=1. */
    mov %rax, %cr0
nm_ucomisd:
    ucomisd %xmm1, %xmm0
    jae fail
    cmpq $1, %r12
    jne fail

    /* 9. #DB single-step must preserve exact flags between TEST and Jcc. */
    movb $'9', %r15b
    xorl %r10d, %r10d
    xorl %eax, %eax
    pushfq
    andq $~0x40, (%rsp)          /* Clear ZF; TEST must set ZF=1. */
    orq $0x100, (%rsp)           /* Set TF=1. */
    popfq
    testq %rax, %rax
single_step_jcc:
    je fail
    cmpq $1, %r10
    jne fail

    /* Print PASS marker and exit cleanly via isa-debug-exit. */
    leaq pass_msg(%rip), %rsi
print_loop:
    movb (%rsi), %al
    testb %al, %al
    je print_done
    outb %al, $0xe9
    incq %rsi
    jmp print_loop
print_done:
    xorl %eax, %eax
    ret

fail:
    movb $'F', %al
    outb %al, $0xe9
    movb %r15b, %al
    outb %al, $0xe9
    movb $'\n', %al
    outb %al, $0xe9
    movl $2, %eax
    outl %eax, $0xf4
    hlt
    jmp fail

debug_handler:
    leaq single_step_jcc(%rip), %rdx
    cmpq %rdx, (%rsp)
    jne fail
    testq $0x40, 16(%rsp)        /* Saved ZF from TEST must be 1. */
    jz fail
    andq $~0x140, 16(%rsp)       /* Clear TF and ZF so resumed JE is not taken. */
    movl $1, %r10d
    iretq

nm_handler:
    leaq nm_ucomisd(%rip), %rdx
    cmpq %rdx, (%rsp)
    jne fail
    clts                         /* Clear CR0.TS so retried UCOMISD succeeds. */
    movl $1, %r12d
    iretq

.section .rodata
.balign 8
fp_one:
    .double 1.0
fp_two:
    .double 2.0
pass_msg:
    .asciz "CPL0_FLAGRDTN_INSTPTN PASS\n"

.data
.balign 16
idt:
    .zero 16 * 16
idtr:
    .word 16 * 16 - 1
    .quad idt
mem_val:
    .long 0
str_src:
    .long 0
str_dst:
    .long 0

.section .note.GNU-stack,"",@progbits
