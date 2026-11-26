/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone 64 KiB firmware. Kernel stacks and GDT use a high canonical alias. */
.section .text
.code16
.Lrom:
#define ROM(x) (0x60000 + (x - .Lrom))
#define HIGH 0xffff800000000000
#define GDT 0x8000
#define IDT 0x9000
#define TSS 0xa000
#define KSTACK (HIGH + 0x28000)
#define USTACK 0x38000
#ifndef IRET_BENCH_ITERATIONS
#define IRET_BENCH_ITERATIONS 100000
#endif
    cli
    cld
    movw %cs,%ax
    movw %ax,%ds
    movw $0x6000,%ax
    movw %ax,%es
    xorw %si,%si
    xorw %di,%di
    movw $0x8000,%cx
    rep movsw
    xorw %ax,%ax
    movw %ax,%ds
    lgdtl %cs:(.Lboot_gdtr-.Lrom)
    movl %cr0,%eax
    orl $1,%eax
    movl %eax,%cr0
    ljmpl $8,$ROM(.Lprotected)
.balign 8
.Lboot_gdt:
    .quad 0,0x00cf9b000000ffff,0x00cf93000000ffff,0x00af9b000000ffff
    .quad 0x00affb000000ffff,0x00cff3000000ffff
    .quad 0x00008900a0000067,0
.Lboot_gdtr:
    .word 63
    .long ROM(.Lboot_gdt)
.code32
.Lprotected:
    movl $16,%eax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movl $0x28000,%esp
    movl $ROM(.Lboot_gdt),%esi
    movl $GDT,%edi
    movl $16,%ecx
    rep movsl
    movl $0x2007,0x1000
    movl $0x2007,0x1800
    movl $0x3007,0x2000
    movl $0x4007,0x3000
    movl $0x4000,%edi
    movl $7,%eax
    movl $512,%ecx
1:  stosl
    movl $0,(%edi)
    addl $4,%edi
    addl $0x1000,%eax
    loop 1b
    movl $0x630,%eax
    movl %eax,%cr4
    movl $0x1000,%eax
    movl %eax,%cr3
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax
    wrmsr
    movl %cr0,%eax
    andl $~4,%eax
    orl $0x80010002,%eax
    movl %eax,%cr0
    ljmpl $24,$ROM(.Llong)
.code64
.macro gate n target dpl=0 ist=0
    movl $ROM(\target),%eax
    movw %ax,IDT+16*\n
    movw $8,IDT+16*\n+2
    movw $(0x8e00+(\dpl<<13)+\ist),IDT+16*\n+4
    shrl $16,%eax
    movw %ax,IDT+16*\n+6
    movq $0,IDT+16*\n+8
.endm
.macro frame target cs=24 ss=16 flags=0x47 sp=KSTACK
    movabsq $\ss,%rax
    pushq %rax
    movabsq $\sp,%rax
    pushq %rax
    pushq $\flags
    pushq $\cs
    pushq $ROM(\target)
.endm
/* Expected faults leave the original IRET/INT instruction and stack visible. */
.macro expect_fault vector error=0 cr2=0 intno=-1
    movl $\vector,.Lexpect_vector(%rip)
    movq $\error,.Lexpect_error(%rip)
    movabsq $\cr2,%rax
    movq %rax,.Lexpect_cr2(%rip)
    leaq 1f(%rip),%rax
    movq %rax,.Lexpect_rip(%rip)
    leaq 2f(%rip),%rax
    movq %rax,.Lresume(%rip)
    movq %rsp,.Lexpect_rsp(%rip)
    pushfq
    popq .Lexpect_flags(%rip)
    movw %ss,%ax
    movw %ax,.Lexpect_ss(%rip)
    movw %cs,%ax
    movw %ax,.Lexpect_cs(%rip)
    movl $0,.Lint_fault_state(%rip)
.if \intno >= 0
    movl $1,.Lint_fault_state(%rip)
    load_state
.endif
    movq $0x123456,%rbx
    movq $0x654321,%r12
1:
.if \intno < 0
    iretq
.else
    int $\intno
.endif
    jmp .Lfail
2:
.endm
.macro load_state
    movq $0x1111,%rax
    movq $0x2222,%rbx
    movq $0x3333,%rcx
    movq $0x4444,%rdx
    movq $0x5555,%rsi
    movq $0x6666,%rdi
    movq $0x7777,%rbp
    movq $0x8888,%r8
    movq $0x9999,%r9
    movq $0xaaaa,%r10
    movq $0xbbbb,%r11
    movq $0xcccc,%r12
    movq $0xdddd,%r13
    movq $0xeeee,%r14
    movq $0xffff,%r15
    movdqu .Lsimd+0(%rip),%xmm0
    movdqu .Lsimd+16(%rip),%xmm1
    movdqu .Lsimd+32(%rip),%xmm2
    movdqu .Lsimd+48(%rip),%xmm3
    movdqu .Lsimd+64(%rip),%xmm4
    movdqu .Lsimd+80(%rip),%xmm5
    movdqu .Lsimd+96(%rip),%xmm6
    movdqu .Lsimd+112(%rip),%xmm7
    movdqu .Lsimd+128(%rip),%xmm8
    movdqu .Lsimd+144(%rip),%xmm9
    movdqu .Lsimd+160(%rip),%xmm10
    movdqu .Lsimd+176(%rip),%xmm11
    movdqu .Lsimd+192(%rip),%xmm12
    movdqu .Lsimd+208(%rip),%xmm13
    movdqu .Lsimd+224(%rip),%xmm14
    movdqu .Lsimd+240(%rip),%xmm15
.endm
.macro check_state fail=.Lfail
    cmpq $0x1111,%rax
    jne \fail
    cmpq $0x2222,%rbx
    jne \fail
    cmpq $0x3333,%rcx
    jne \fail
    cmpq $0x4444,%rdx
    jne \fail
    cmpq $0x5555,%rsi
    jne \fail
    cmpq $0x6666,%rdi
    jne \fail
    cmpq $0x7777,%rbp
    jne \fail
    cmpq $0x8888,%r8
    jne \fail
    cmpq $0x9999,%r9
    jne \fail
    cmpq $0xaaaa,%r10
    jne \fail
    cmpq $0xbbbb,%r11
    jne \fail
    cmpq $0xcccc,%r12
    jne \fail
    cmpq $0xdddd,%r13
    jne \fail
    cmpq $0xeeee,%r14
    jne \fail
    cmpq $0xffff,%r15
    jne \fail
    pcmpeqb .Lsimd+0(%rip),%xmm0
    pmovmskb %xmm0,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+16(%rip),%xmm1
    pmovmskb %xmm1,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+32(%rip),%xmm2
    pmovmskb %xmm2,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+48(%rip),%xmm3
    pmovmskb %xmm3,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+64(%rip),%xmm4
    pmovmskb %xmm4,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+80(%rip),%xmm5
    pmovmskb %xmm5,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+96(%rip),%xmm6
    pmovmskb %xmm6,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+112(%rip),%xmm7
    pmovmskb %xmm7,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+128(%rip),%xmm8
    pmovmskb %xmm8,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+144(%rip),%xmm9
    pmovmskb %xmm9,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+160(%rip),%xmm10
    pmovmskb %xmm10,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+176(%rip),%xmm11
    pmovmskb %xmm11,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+192(%rip),%xmm12
    pmovmskb %xmm12,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+208(%rip),%xmm13
    pmovmskb %xmm13,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+224(%rip),%xmm14
    pmovmskb %xmm14,%eax
    cmpl $65535,%eax
    jne \fail
    pcmpeqb .Lsimd+240(%rip),%xmm15
    pmovmskb %xmm15,%eax
    cmpl $65535,%eax
    jne \fail
.endm
.macro case n
    movl $\n,.Lcase(%rip)
.endm
.Llong:
    movabsq $KSTACK,%rsp
    lgdt .Lgdtr(%rip)
    /* Keep a separate, already-accessed handler CS before split-GDT tests. */
    movabsq $0x00af9b000000ffff,%rax
    movq %rax,GDT+8
    movabsq $KSTACK,%rax
    movq %rax,TSS+4
    movabsq $(HIGH+0x30000),%rax
    movq %rax,TSS+36
    movw $104,TSS+102
    movl $48,%eax
    ltr %ax
    gate 6 .Lud 0 1
    gate 11 .Lnp 0 1
    gate 12 .Lss 0 1
    gate 13 .Lgp 0 1
    gate 14 .Lpf 0 1
    gate 2 .Lnmi 0 2
    movw $24,IDT+16*2+2
    gate 0x81 .Lirq
    movw $24,IDT+16*0x81+2
    movabsq $(HIGH+0x32000),%rax
    movq %rax,TSS+44
    gate 0x80 .Luser_back 3
    movw $24,IDT+16*0x80+2
    lidt .Lidtr(%rip)
    movb $0xff,%al
    outb %al,$0x21
    outb %al,$0xa1
#ifdef IRET_INT_LOOP
    /* No IRET or ordinary branch in this loop: INT's own exit must poll. */
    gate 0x80 .Lint_exit_loop
    movw $24,IDT+16*0x80+2
    leaq .Lint_exit_text(%rip),%rsi
    call .Lputs
.Lint_exit_loop:
    movabsq $KSTACK,%rsp
    int $0x80
    ud2
.Lint_exit_text: .asciz "IRET INT LOOP\n"
#endif
#ifdef IRET_BENCH_ONLY
    leaq .Lbench_only_text(%rip),%rsi
    call .Lputs
    jmp .Ltests_more
#endif
    case 1
    frame .Lkernel_nonnull
    load_state
    iretq
.Lkernel_nonnull:
    pushfq
    popq .Lreturned_flags(%rip)
    check_state
    movq .Lreturned_flags(%rip),%rax
    andl $0xcd5,%eax
    cmpl $0x45,%eax
    jne .Lfail
    movw %ss,%ax
    cmpw $16,%ax
    jne .Lfail
    movabsq $KSTACK,%rax
    cmpq %rax,%rsp
    jne .Lfail
    case 2
    frame .Lkernel_null 24 0
    iretq
.Lkernel_null:
    movw %ss,%ax
    testw %ax,%ax
    jne .Lfail
    case 3
    frame .Luser 35 43 0x47 USTACK
    load_state
    iretq
.Luser:
    pushfq
    popq .Lreturned_flags(%rip)
    cmpq $USTACK,%rsp
    jne .Luser_fail
    check_state .Luser_fail
    movq .Lreturned_flags(%rip),%rax
    andl $0xcd5,%eax
    cmpl $0x45,%eax
    jne .Luser_fail
    movw %cs,%ax
    cmpw $35,%ax
    jne .Luser_fail
    movw %ss,%ax
    cmpw $43,%ax
    jne .Luser_fail
    /* A user-mode IRET must take the ordinary architectural path. */
    pushq $43
    pushq $USTACK
    pushq $0x47
    pushq $35
    pushq $ROM(.Luser_iret)
    iretq
.Luser_iret:
    cmpq $USTACK,%rsp
    jne .Luser_fail
    xorl %eax,%eax
    int $0x80
.Luser_fail:
    movl $1,%eax
    int $0x80
.Luser_back:
    cmpl $1,.Luser_bench(%rip)
    je .Luser_bench_back
    testl %eax,%eax
    jne .Lfail
    cmpl $1,.Luser_stage(%rip)
    je .Lsegment_done
    cmpl $2,.Luser_stage(%rip)
    je .Lmpx_done
    cmpl $3,.Luser_stage(%rip)
    je .Lconforming_done
    cmpl $4,.Luser_stage(%rip)
    je .Lint_user_done
    cmpl $5,.Luser_stage(%rip)
    je .Lint_tb_done
    movabsq $KSTACK,%rsp
    case 4
    frame .Lunexpected 0 16
    expect_fault 13 0
    case 5
    frame .Lunexpected 16 16
    expect_fault 13 16
    case 6
    frame .Lunexpected 35 0
    expect_fault 13 0
    case 7
    frame .Lunexpected 35 16
    expect_fault 13 16
    case 8
    frame .Lunexpected 24 24
    expect_fault 13 24
    case 9
    /* A nonpresent code descriptor must report its selector. */
    andb $0x7f,GDT+32+5
    frame .Lunexpected 35 43
    expect_fault 11 32
    orb $0x80,GDT+32+5
    case 10
    andb $0x7f,GDT+40+5
    frame .Lunexpected 35 43
    expect_fault 11 40
    orb $0x80,GDT+40+5
    case 11
    /* Invalid descriptor index, outside the GDT limit. */
    frame .Lunexpected 0x80 16
    expect_fault 13 0x80
    case 12
    /* A five-qword IRET frame crossing a present page is valid. */
    frame .Lsplit_return
    movq %rsp,%rsi
    movl $0x43fe0,%edi
    movl $5,%ecx
    rep movsq
    movabsq $(HIGH+0x43fe0),%rsp
    iretq
.Lsplit_return:
    case 13
    frame .Lunexpected
    movq %rsp,%rsi
    movl $0x43fe0,%edi
    movl $5,%ecx
    rep movsq
    movq $0,0x4000+8*0x44
    movabsq $(HIGH+0x44000),%rax
    invlpg (%rax)
    movabsq $(HIGH+0x43fe0),%rsp
    expect_fault 14 0 (HIGH+0x44000)
    case 14
    /* Readable ROM code, but a supervisor-protected frame page. SMAP/AC
     * are deliberately absent: plain supervisor reads remain permitted. */
    frame .Lprotected_return
    andq $~2,0x4000+8*0x27
    movabsq $(HIGH+0x27000),%rax
    invlpg (%rax)
    iretq
.Lprotected_return:
    orq $2,0x4000+8*0x27
    movabsq $(HIGH+0x27000),%rax
    invlpg (%rax)
    case 15
    /* With current NT set, long-mode IRET faults before committing state. */
    frame .Lunexpected
    pushfq
    orq $0x4000,(%rsp)
    popfq
    expect_fault 13 0
    case 16
    /* Return flags requiring the helper: VM is ignored in long mode;
     * AC/ID/IOPL are restored for a CPL0 caller. */
    frame .Lflags_return 24 16 0x273047
    iretq
.Lflags_return:
    pushfq
    popq %rax
    andl $0x273045,%eax
    cmpl $0x243045,%eax
    jne .Lfail
    pushq $2
    popfq
    case 17
    /* A code descriptor split across two present pages is valid. */
    movl $GDT,%esi
    movl $0x40fe4,%edi
    movl $8,%ecx
    rep movsq
    lgdt .Lsplit_gdtr(%rip)
    frame .Lgdt_split_return
    iretq
.Lgdt_split_return:
    case 18
    frame .Lunexpected
    movq $0,0x4000+8*0x41
    movabsq $(HIGH+0x41000),%rax
    invlpg (%rax)
    expect_fault 14 0 (HIGH+0x41000)
    case 19
    /* Only SS straddles the missing page; CS is wholly readable. */
    movl $GDT,%esi
    movl $0x40fd4,%edi
    movl $8,%ecx
    rep movsq
    lgdt .Lss_split_gdtr(%rip)
    frame .Lunexpected 35 43
    movq $0,0x4000+8*0x41
    movabsq $(HIGH+0x41000),%rax
    invlpg (%rax)
    expect_fault 14 0 (HIGH+0x41000)
    case 20
    /* High-page protection must not be accidentally bypassed by a low
     * physical alias in the translator. Leave the frame nonpresent. */
    frame .Lunexpected
    movq %rsp,%rsi
    movl $0x45000,%edi
    movl $5,%ecx
    rep movsq
    movq $0,0x4000+8*0x45
    movabsq $(HIGH+0x45000),%rax
    invlpg (%rax)
    movq %rax,%rsp
    expect_fault 14 0 (HIGH+0x45000)
    case 21
    /* CS validation precedes fetching the RSP/SS tail of the frame. */
    frame .Lunexpected 0 16
    movq %rsp,%rsi
    movl $0x43fe0,%edi
    movl $5,%ecx
    rep movsq
    movq $0,0x4000+8*0x44
    movabsq $(HIGH+0x44000),%rax
    invlpg (%rax)
    movabsq $(HIGH+0x43fe0),%rsp
    expect_fault 13 0
    case 22
    /* A readable but nonwritable data selector is invalid for SS. */
    andb $~2,GDT+40+5
    frame .Lunexpected 35 43
    expect_fault 13 40
    orb $2,GDT+40+5
    case 23
    /* An ordinary code segment's DPL must match its requested CPL. */
    andb $~0x60,GDT+32+5
    frame .Lunexpected 35 43
    expect_fault 13 32
    orb $0x60,GDT+32+5
    case 24
    /* RIP/CS/RFLAGS are read before CS validation: missing RFLAGS wins. */
    frame .Lunexpected 0 16
    movq %rsp,%rsi
    movl $0x43ff0,%edi
    movl $5,%ecx
    rep movsq
    movq $0,0x4000+8*0x44
    movabsq $(HIGH+0x44000),%rax
    invlpg (%rax)
    movabsq $(HIGH+0x43ff0),%rsp
    expect_fault 14 0 (HIGH+0x44000)
    movl $0,.Lexpect_vector(%rip)
    case 25
    /* Map the local APIC MMIO page at an otherwise unused RAM address. */
    movl $0xfee00003,0x4000+8*0x50
    invlpg 0x50000
    movl $0x1ff,0x500f0
    movl $0,.Lnmi_seen(%rip)
    movl $0x44400,0x50300
    movl $10000000,%ecx
.Lnmi_wait:
    cmpl $2,.Lnmi_seen(%rip)
    je .Lnmi_done
    pause
    loop .Lnmi_wait
    jmp .Lfail
.Lnmi_done:
    case 26
    cli
    movl $0,.Lirq_seen(%rip)
    movl $0x44081,0x50300
    /* Pending APIC IRQ cannot arrive until IRET restores IF. */
    movl $1000,%ecx
1:  pause
    loop 1b
    cmpl $0,.Lirq_seen(%rip)
    jne .Lfail
    frame .Lirq_wait 24 16 0x247
    iretq
.Lirq_wait:
    movl $10000000,%ecx
1:  cmpl $1,.Lirq_seen(%rip)
    je .Lirq_done
    pause
    loop 1b
    jmp .Lfail
.Lirq_done:
    cli
    case 27
    /* Null FS/GS retain their 64-bit base when CPL3 invalidates DS/ES. */
    movl $16,%eax
    movw %ax,%ds
    movw %ax,%es
    xorl %eax,%eax
    movw %ax,%fs
    movw %ax,%gs
    movl $0x47000,%eax
    xorl %edx,%edx
    movl $0xc0000100,%ecx
    wrmsr
    movl $0x48000,%eax
    incl %ecx
    wrmsr
    movl $0x12345678,0x47000
    movl $0x76543210,0x48000
    movl $1,.Luser_stage(%rip)
    frame .Lsegment_user 35 43 2 USTACK
    iretq
.Lsegment_user:
    movw %ds,%ax
    testw %ax,%ax
    jne .Luser_fail
    movw %es,%ax
    testw %ax,%ax
    jne .Luser_fail
    movw %fs,%ax
    testw %ax,%ax
    jne .Luser_fail
    movw %gs,%ax
    testw %ax,%ax
    jne .Luser_fail
    movl %fs:0,%eax
    cmpl $0x12345678,%eax
    jne .Luser_fail
    movl %gs:0,%eax
    cmpl $0x76543210,%eax
    jne .Luser_fail
    xorl %eax,%eax
    int $0x80
.Lsegment_done:
    movabsq $KSTACK,%rsp
    movl $0,.Luser_stage(%rip)
    xorl %eax,%eax
    xorl %edx,%edx
    movl $0xc0000100,%ecx
    wrmsr
    incl %ecx
    wrmsr
    case 30
    /* Unlike case 27, a DPL0 conforming readable code segment remains
     * usable in data-segment registers after returning to CPL3. */
    orb $4,GDT+8+5
    movl $8,%eax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%fs
    movw %ax,%gs
    movl $0x47000,%eax
    xorl %edx,%edx
    movl $0xc0000100,%ecx
    wrmsr
    movl $0x48000,%eax
    incl %ecx
    wrmsr
    movl $3,.Luser_stage(%rip)
    frame .Lconforming_user 35 43 2 USTACK
    iretq
.Lconforming_user:
    movw %ds,%ax
    cmpw $8,%ax
    jne .Luser_fail
    movw %es,%ax
    cmpw $8,%ax
    jne .Luser_fail
    movw %fs,%ax
    cmpw $8,%ax
    jne .Luser_fail
    movw %gs,%ax
    cmpw $8,%ax
    jne .Luser_fail
    movl %fs:0,%eax
    cmpl $0x12345678,%eax
    jne .Luser_fail
    movl %gs:0,%eax
    cmpl $0x76543210,%eax
    jne .Luser_fail
    xorl %eax,%eax
    int $0x80
.Lconforming_done:
    movabsq $KSTACK,%rsp
    movl $0,.Luser_stage(%rip)
    movl $16,%eax
    movw %ax,%ds
    movw %ax,%es
    xorl %eax,%eax
    movw %ax,%fs
    movw %ax,%gs
    andb $~4,GDT+8+5
    xorl %edx,%edx
    movl $0xc0000100,%ecx
    wrmsr
    incl %ecx
    wrmsr
.Lint_start:
    movl $16,%eax
    movw %ax,%ss
    /* INT must preserve guest registers and build a five-qword long-mode
     * frame, including for vectors normally associated with error codes. */
    case 31
    gate 0x7f .Lint_entry 3
    movabsq $(KSTACK-40),%rax
    movq %rax,.Lint_stack(%rip)
    movabsq $KSTACK,%rax
    movq %rax,.Lint_old_rsp(%rip)
    movq $24,.Lint_old_cs(%rip)
    movq $16,.Lint_old_ss(%rip)
    movq $0,.Lint_handler_if(%rip)
    leaq .Lint_kernel_return(%rip),%rax
    movq %rax,.Lint_return(%rip)
    pushq $0x247
    popfq
    load_state
    int $0x7f
.Lint_kernel_return:
    pushfq
    popq .Lreturned_flags(%rip)
    check_state
    cmpq $0x247,.Lreturned_flags(%rip)
    jne .Lfail
    cli
    case 32
    /* Trap gates preserve IF; interrupt gates clear it in the handler. */
    movb $0xef,IDT+16*0x7f+5
    movq $0x200,.Lint_handler_if(%rip)
    leaq .Lint_trap_return(%rip),%rax
    movq %rax,.Lint_return(%rip)
    pushq $0x247
    popfq
    load_state
    int $0x7f
.Lint_trap_return:
    pushfq
    popq .Lreturned_flags(%rip)
    check_state
    cmpq $0x247,.Lreturned_flags(%rip)
    jne .Lfail
    cli
    case 33
    gate 0x7f .Lint_entry 3
    movq $0,.Lint_handler_if(%rip)
    movq $USTACK,.Lint_old_rsp(%rip)
    movq $35,.Lint_old_cs(%rip)
    movq $43,.Lint_old_ss(%rip)
    leaq .Lint_user_return(%rip),%rax
    movq %rax,.Lint_return(%rip)
    movl $4,.Luser_stage(%rip)
    frame .Lint_user 35 43 0x247 USTACK
    iretq
.Lint_user:
    load_state
    int $0x7f
.Lint_user_return:
    pushfq
    popq .Lreturned_flags(%rip)
    check_state .Luser_fail
    cmpq $0x247,.Lreturned_flags(%rip)
    jne .Luser_fail
    cmpq $USTACK,%rsp
    jne .Luser_fail
    xorl %eax,%eax
    int $0x80
.Lint_user_done:
    movl $16,%eax
    movw %ax,%ss
    movabsq $KSTACK,%rsp
    movl $0,.Luser_stage(%rip)
    cli
    case 34
    /* An IST switch is required even when no privilege change occurs. */
    movabsq $(HIGH+0x34000),%rax
    movq %rax,TSS+52
    gate 0x7f .Lint_entry 3 3
    movabsq $(HIGH+0x34000-40),%rax
    movq %rax,.Lint_stack(%rip)
    movabsq $KSTACK,%rax
    movq %rax,.Lint_old_rsp(%rip)
    movq $24,.Lint_old_cs(%rip)
    movq $16,.Lint_old_ss(%rip)
    leaq .Lint_ist_return(%rip),%rax
    movq %rax,.Lint_return(%rip)
    pushq $0x247
    popfq
    load_state
    int $0x7f
.Lint_ist_return:
    check_state
    movabsq $KSTACK,%rax
    cmpq %rax,%rsp
    jne .Lfail
    cli
    case 35
    gate 8 .Lint_entry
    movabsq $(KSTACK-40),%rax
    movq %rax,.Lint_stack(%rip)
    leaq .Lint_eight_return(%rip),%rax
    movq %rax,.Lint_return(%rip)
    pushq $0x247
    popfq
    load_state
    int $8
.Lint_eight_return:
    check_state
    cli
    case 36
    gate 14 .Lint_entry
    leaq .Lint_fourteen_return(%rip),%rax
    movq %rax,.Lint_return(%rip)
    pushq $0x247
    popfq
    load_state
    int $14
.Lint_fourteen_return:
    check_state
    cli
    gate 14 .Lpf 0 1
    case 37
    gate 0x7f .Lint_entry 3
    movb $0xe0,IDT+16*0x7f+5
    expect_fault 13 0x3fa 0 0x7f
    case 38
    movb $0x6e,IDT+16*0x7f+5
    expect_fault 11 0x3fa 0 0x7f
    case 39
    movb $0xee,IDT+16*0x7f+5
    lidt .Lint_short_idtr(%rip)
    expect_fault 13 0x3fa 0 0x7f
    lidt .Lidtr(%rip)
    case 40
    movw $0,IDT+16*0x7f+2
    expect_fault 13 0 0 0x7f
    case 41
    movw $16,IDT+16*0x7f+2
    expect_fault 13 16 0 0x7f
    case 42
    movw $24,IDT+16*0x7f+2
    andb $0x7f,GDT+24+5
    expect_fault 11 24 0 0x7f
    orb $0x80,GDT+24+5
    case 43
    /* A CPL3 INT through a DPL0 gate faults at the INT, before any stack
     * write; recovery checks the user CS/SS/RSP and resumes in the kernel. */
    gate 0x7f .Lint_entry
    frame .Lint_dpl_user 35 43 2 USTACK
    iretq
.Lint_dpl_user:
    expect_fault 13 0x3fa 0 0x7f
    case 44
    gate 0x7f .Lint_entry 3
    movabsq $(HIGH+0x46000),%rax
    movq %rax,TSS+4
    movq $0,0x4000+8*0x45
    movabsq $(HIGH+0x45000),%rax
    invlpg (%rax)
    frame .Lint_stack_user 35 43 2 USTACK
    iretq
.Lint_stack_user:
    expect_fault 14 2 (HIGH+0x45ff8) 0x7f
    case 45
    /* Gate validation precedes touching a missing privilege-change stack. */
    movq $0,0x4000+8*0x45
    movabsq $(HIGH+0x45000),%rax
    invlpg (%rax)
    movb $0xe0,IDT+16*0x7f+5
    frame .Lint_priority_user 35 43 2 USTACK
    iretq
.Lint_priority_user:
    expect_fault 13 0x3fa 0 0x7f
    case 46
    /* DPL failure wins over the not-present bit for a software INT. */
    gate 0x7f .Lint_entry
    andb $0x7f,IDT+16*0x7f+5
    frame .Lint_dpl_np_user 35 43 2 USTACK
    iretq
.Lint_dpl_np_user:
    expect_fault 13 0x3fa 0 0x7f
    case 47
    /* A missing IST stack faults without changing the interrupted CPL0
     * frame. The page-fault handler has its own, independent IST stack. */
    gate 0x7f .Lint_entry 3 3
    movabsq $(HIGH+0x46000),%rax
    movq %rax,TSS+52
    movq $0,0x4000+8*0x45
    movabsq $(HIGH+0x45000),%rax
    invlpg (%rax)
    expect_fault 14 2 (HIGH+0x45ff8) 0x7f
    case 48
    /* INT cannot write TSS.RSP0, and #PF cannot write that same stack.
     * Delivery must escalate to #DF on a separate, valid IST stack. */
    gate 8 .Ldf 0 1
    gate 14 .Lpf
    gate 0x7f .Lint_entry 3
    movabsq $(HIGH+0x46000),%rax
    movq %rax,TSS+4
    movq $0,0x4000+8*0x45
    movabsq $(HIGH+0x45000),%rax
    invlpg (%rax)
    frame .Lint_double_fault_user 35 43 2 USTACK
    iretq
.Lint_double_fault_user:
    expect_fault 8 0 0 0x7f
    gate 14 .Lpf 0 1
    movabsq $KSTACK,%rax
    movq %rax,TSS+4
    case 49
    /* A software INT is not an exception already in progress. If its
     * own gate is invalid, #GP must not be mistaken for a second fault. */
    movb $0x80,IDT+16*8+5
    expect_fault 13 0x42 0 8
    gate 8 .Ldf 0 1
    case 50
    movb $0x80,IDT+16*14+5
    expect_fault 13 0x72 0 14
    gate 14 .Lpf 0 1
.Lint_page_start:
    case 51
    /* Prime a CPL3 translation of CLI at the exact low-address handler PC.
     * Keep the code and CR3 unchanged between this #GP and the real INT. */
    leaq .Lint_tb_code(%rip),%rsi
    movl $0x70000,%edi
    movl $(.Lint_tb_code_end-.Lint_tb_code),%ecx
    rep movsb
    movw $0x7fcd,0x71ffe
    /* On IRET, clear EAX and use the ordinary user-to-kernel continuation. */
    movl $0x80cdc031,0x72000
    movl $13,.Lexpect_vector(%rip)
    movq $0,.Lexpect_error(%rip)
    movq $0x70000,.Lexpect_rip(%rip)
    movq $USTACK,.Lexpect_rsp(%rip)
    movq $2,.Lexpect_flags(%rip)
    movw $43,.Lexpect_ss(%rip)
    movw $35,.Lexpect_cs(%rip)
    movl $1,.Lint_fault_state(%rip)
    leaq .Lint_tb_seed_done(%rip),%rax
    movq %rax,.Lresume(%rip)
    frame .Lint_tb_seed_user 35 43 2 USTACK
    iretq
.Lint_tb_seed_user:
    load_state
    movq $0x123456,%rbx
    movq $0x654321,%r12
    pushq $0x70000
    ret
.Lint_tb_seed_done:
    movl $0,.Lexpect_vector(%rip)
    movl $0,.Lint_fault_state(%rip)
    gate 0x7f .Lint_entry 3
    /* 0x70000 is unchanged: the first instruction is the primed CLI. */
    movw $0,IDT+16*0x7f
    movw $24,IDT+16*0x7f+2
    movw $7,IDT+16*0x7f+6
    movl $5,.Luser_stage(%rip)
    frame .Lint_tb_invoke 35 43 0x247 USTACK
    iretq
.Lint_tb_invoke:
    load_state
    /* The two-byte INT ends exactly at a 4 KiB boundary. */
    pushq $0x71ffe
    ret
.Lint_tb_kernel:
    popq %rax
    check_state
    movw %cs,%ax
    cmpw $24,%ax
    jne .Lfail
    movw %ss,%ax
    testw %ax,%ax
    jne .Lfail
    movabsq $(KSTACK-40),%rax
    cmpq %rax,%rsp
    jne .Lfail
    cmpq $0x72000,(%rsp)
    jne .Lfail
    cmpq $35,8(%rsp)
    jne .Lfail
    cmpq $0x247,16(%rsp)
    jne .Lfail
    cmpq $USTACK,24(%rsp)
    jne .Lfail
    cmpq $43,32(%rsp)
    jne .Lfail
    load_state
    iretq
.Lint_tb_done:
    movabsq $KSTACK,%rsp
    movl $16,%eax
    movw %ax,%ss
    movl $0,.Luser_stage(%rip)
    movl $0,.Lexpect_vector(%rip)
    leaq .Lint_pass_text(%rip),%rsi
    call .Lputs
.Lmpx_start:
    /* MPX checks are conditional on CPUID, never silently emulated in the
     * firmware. CPUs without XSAVE+MPX report an explicit skip. */
    movl $1,%eax
    cpuid
    btl $26,%ecx
    jnc .Lmpx_skip
    movl $7,%eax
    xorl %ecx,%ecx
    cpuid
    btl $14,%ebx
    jnc .Lmpx_skip
    movq %cr4,%rax
    movq %rax,.Lold_cr4(%rip)
    orq $0x40000,%rax
    movq %rax,%cr4
    xorl %ecx,%ecx
    xgetbv
    movl %eax,.Lold_xcr0(%rip)
    movl %edx,.Lold_xcr0+4(%rip)
    movl $0x1b,%eax
    xorl %edx,%edx
    xsetbv
    movl $0,.Lmpx_phase(%rip)
.Lmpx_round:
    case 28
    movl $1,%eax
    movq $3,0x5400
    movq $0x100,.Lmpx_expected(%rip)
    cmpl $0,.Lmpx_phase(%rip)
    je 1f
    case 29
    movl $3,%eax
    movq $1,0x5400
    movq $0,.Lmpx_expected(%rip)
1:  xorl %edx,%edx
    movl $0xd90,%ecx
    wrmsr
    movq $0x18,0x5200
    movq $0x100,0x53c0
    movq $-0x200,0x53c8
    movl $0x18,%eax
    xorl %edx,%edx
    xrstor 0x5000
    movl $2,.Luser_stage(%rip)
    frame .Lmpx_user 35 43 2 USTACK
    iretq
.Lmpx_user:
    /* A legacy, non-BND-prefixed branch obeys the user BNDPRESERVE bit,
     * even when the supervisor setting before IRET was the opposite. */
    cmpl %eax,%eax
    je .Lmpx_user_after_jump
.Lmpx_user_after_jump:
    movl $0x18,%eax
    xorl %edx,%edx
    xsave 0x5800
    movq 0x5bc0,%rax
    cmpq .Lmpx_expected(%rip),%rax
    jne .Luser_fail
    movq 0x5c00,%rax
    cmpq 0x5400,%rax
    jne .Luser_fail
    xorl %eax,%eax
    int $0x80
.Lmpx_done:
    movabsq $KSTACK,%rsp
    cmpl $0,.Lmpx_phase(%rip)
    jne .Lmpx_cleanup
    movl $1,.Lmpx_phase(%rip)
    jmp .Lmpx_round
.Lmpx_cleanup:
    movl $0,.Luser_stage(%rip)
    xorl %eax,%eax
    xorl %edx,%edx
    movl $0xd90,%ecx
    wrmsr
    movq $0,0x5400
    movq $0,0x53c0
    movq $0,0x53c8
    movl $0x18,%eax
    xrstor 0x5000
    xorl %ecx,%ecx
    movl .Lold_xcr0(%rip),%eax
    movl .Lold_xcr0+4(%rip),%edx
    xsetbv
    movq .Lold_cr4(%rip),%rax
    movq %rax,%cr4
    leaq .Lmpx_pass_text(%rip),%rsi
    call .Lputs
    jmp .Ltests_more
.Lmpx_skip:
    leaq .Lmpx_skip_text(%rip),%rsi
    call .Lputs
    jmp .Ltests_more
.Lunexpected:
    jmp .Lfail
.Ltests_more:
    movl $IRET_BENCH_ITERATIONS,%eax
    movq %rax,%rbx
    leaq .Literations_text(%rip),%rsi
    call .Lputs
    movq %rbx,%rax
    call .Lhex
    /* Benchmark a real CPL0 -> CPL0 IRET loop, reporting raw TSC ticks. */
    movl $IRET_BENCH_ITERATIONS,%ecx
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    movq %rax,.Lstart(%rip)
.Lbench:
    frame .Lbench_return
    iretq
.Lbench_return:
    decl %ecx
    jnz .Lbench
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    subq .Lstart(%rip),%rax
    movq %rax,%rbx
    leaq .Lbench_text(%rip),%rsi
    call .Lputs
    movq %rbx,%rax
    call .Lhex
    /* The user loop includes a real software interrupt and kernel IRET. */
    movl $1,.Luser_bench(%rip)
    movl $IRET_BENCH_ITERATIONS,.Lremaining(%rip)
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    movq %rax,.Lstart(%rip)
    frame .Luser_bench_loop 35 43 0x47 USTACK
    iretq
.Luser_bench_loop:
    int $0x80
    jmp .Luser_bench_loop
.Luser_bench_back:
    decl .Lremaining(%rip)
    jz .Luser_bench_done
    iretq
.Luser_bench_done:
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    subq .Lstart(%rip),%rax
    movq %rax,%rbx
    movabsq $KSTACK,%rsp
    leaq .Luser_bench_text(%rip),%rsi
    call .Lputs
    movq %rbx,%rax
    call .Lhex
    leaq .Lpass_text(%rip),%rsi
    call .Lputs
    movl $0x10,%eax
    outl %eax,$0xf4
    jmp .Lhalt
.Lint_tb_code:
    cli
    pushq %rax
    movabsq $ROM(.Lint_tb_kernel),%rax
    jmp *%rax
.Lint_tb_code_end:
.Lint_entry:
    pushfq
    popq .Lint_entry_flags(%rip)
    check_state
    cmpq .Lint_stack(%rip),%rsp
    jne .Lfail
    movq .Lint_return(%rip),%rax
    cmpq %rax,(%rsp)
    jne .Lfail
    movq .Lint_old_cs(%rip),%rax
    cmpq %rax,8(%rsp)
    jne .Lfail
    cmpq $0x247,16(%rsp)
    jne .Lfail
    movq .Lint_old_rsp(%rip),%rax
    cmpq %rax,24(%rsp)
    jne .Lfail
    movq .Lint_old_ss(%rip),%rax
    cmpq %rax,32(%rsp)
    jne .Lfail
    movq .Lint_entry_flags(%rip),%rax
    andq $0x200,%rax
    cmpq .Lint_handler_if(%rip),%rax
    jne .Lfail
    load_state
    iretq
.Lnmi:
    pushq %rax
    pushq %rcx
    cmpl $0,.Lin_nmi(%rip)
    jne .Lfail
    movl $1,.Lin_nmi(%rip)
    incl .Lnmi_seen(%rip)
    cmpl $1,.Lnmi_seen(%rip)
    jne 2f
    /* A second self-NMI remains blocked until the first IRET completes. */
    movl $0x44400,0x50300
    movl $1000,%ecx
1:  pause
    loop 1b
    cmpl $1,.Lnmi_seen(%rip)
    jne .Lfail
2:  movl $0,.Lin_nmi(%rip)
    popq %rcx
    popq %rax
    iretq
.Lirq:
    testq $0x200,16(%rsp)
    jz .Lfail
    incl .Lirq_seen(%rip)
    movl $0,0x500b0
    iretq
.Lud:
    pushq $0
    pushq $6
    jmp .Ltrap
.Ldf:
    pushq $8
    jmp .Ltrap
.Lnp:
    pushq $11
    jmp .Ltrap
.Lss:
    pushq $12
    jmp .Ltrap
.Lgp:
    pushq $13
    jmp .Ltrap
.Lpf:
    pushq $14
.Ltrap:
    movq %rax,.Lfault_rax(%rip)
    movq (%rsp),%rax
    movq %rax,.Lvector(%rip)
    movq 8(%rsp),%rax
    movq %rax,.Lerror(%rip)
    movq 16(%rsp),%rax
    movq %rax,.Lfault_rip(%rip)
    /* Case 51 has no fixture changes. Preserve the primed user TB by
     * avoiding page-table/CR3 writes during its expected CLI #GP recovery. */
    cmpl $51,.Lcase(%rip)
    je .Ltrap_fixture_ready
    lgdt .Lgdtr(%rip)
    /* Restore fixture mappings before examining the saved state. */
    movq $0x41007,0x4000+8*0x41
    movq $0x44007,0x4000+8*0x44
    movq $0x45007,0x4000+8*0x45
    movq %cr3,%rax
    movq %rax,%cr3
.Ltrap_fixture_ready:
    movl .Lexpect_vector(%rip),%eax
    testl %eax,%eax
    jz .Lfail
    cmpq (%rsp),%rax
    jne .Lfail
    movq .Lexpect_error(%rip),%rax
    cmpq 8(%rsp),%rax
    jne .Lfail
    /* Double fault is an abort: its saved RIP is architecturally
     * undefined. All ordinary fault cases still require the exact INT/IRET. */
    cmpl $8,.Lexpect_vector(%rip)
    je 3f
    movq .Lexpect_rip(%rip),%rax
    cmpq 16(%rsp),%rax
    jne .Lfail
3:  movzwq .Lexpect_cs(%rip),%rax
    cmpq %rax,24(%rsp)
    jne .Lfail
    movq .Lexpect_rsp(%rip),%rax
    cmpq 40(%rsp),%rax
    jne .Lfail
    movzwq .Lexpect_ss(%rip),%rax
    cmpq 48(%rsp),%rax
    jne .Lfail
    /* RF is set by fault delivery, all other saved flags remain intact. */
    movq 32(%rsp),%rax
    andq $~0x10000,%rax
    cmpq .Lexpect_flags(%rip),%rax
    jne .Lfail
    cmpq $0x123456,%rbx
    jne .Lfail
    cmpq $0x654321,%r12
    jne .Lfail
    cmpl $14,.Lexpect_vector(%rip)
    jne 1f
    movq %cr2,%rax
    cmpq .Lexpect_cr2(%rip),%rax
    jne .Lfail
1:  cmpl $0,.Lint_fault_state(%rip)
    je 2f
    /* All software-INT fault paths preserve the full incoming integer and
     * SIMD state. The common trap machinery saved RAX before using it. */
    movq .Lfault_rax(%rip),%rax
    movq $0x2222,%rbx
    movq $0xcccc,%r12
    check_state
2:  movabsq $KSTACK,%rsp
    orb $0x80,GDT+24+5
    /* Restore the tested CS without invoking the instruction under test. */
    pushq $24
    pushq .Lresume(%rip)
    lretq
.Lfail:
    leaq .Lfail_text(%rip),%rsi
    call .Lputs
    movl .Lcase(%rip),%eax
    call .Lhex
    movq .Lvector(%rip),%rax
    call .Lhex
    movq .Lerror(%rip),%rax
    call .Lhex
    movq .Lfault_rip(%rip),%rax
    call .Lhex
    movl $0x11,%eax
    outl %eax,$0xf4
.Lhalt:
    sti
.Lwait:
    /* Tests and timing are complete. Enable interrupt polling so LATS can
     * acknowledge an external QMP stop when port 0xf4 is not implemented. */
    pause
    jmp .Lwait
.Lputs:
    lodsb
    testb %al,%al
    jz 1f
    outb %al,$0xe9
    jmp .Lputs
1:  ret
.Lhex:
    movq %rax,%rdx
    movl $16,%ecx
1:  rolq $4,%rdx
    movl %edx,%eax
    andl $15,%eax
    addb $48,%al
    cmpb $57,%al
    jbe 2f
    addb $39,%al
2:  outb %al,$0xe9
    loop 1b
    movb $10,%al
    outb %al,$0xe9
    ret
.Lgdtr:
    .word 63
    .quad HIGH+GDT
.Lidtr:
    .word 16*130-1
    .quad IDT
.Lsplit_gdtr:
    .word 63
    .quad HIGH+0x40fe4
.Lss_split_gdtr:
    .word 63
    .quad HIGH+0x40fd4
.Lexpect_vector: .long 0
.Lexpect_error: .quad 0
.Lexpect_rip: .quad 0
.Lexpect_rsp: .quad 0
.Lexpect_flags: .quad 0
.Lexpect_cr2: .quad 0
.Lexpect_ss: .word 0
.Lexpect_cs: .word 0
.Lfault_rax: .quad 0
.Lint_fault_state: .long 0
.Lint_short_idtr:
    .word 16*127-1
    .quad IDT
.Lint_stack: .quad 0
.Lint_return: .quad 0
.Lint_old_cs: .quad 0
.Lint_old_ss: .quad 0
.Lint_old_rsp: .quad 0
.Lint_handler_if: .quad 0
.Lint_entry_flags: .quad 0
.Lresume: .quad 0
.Lold_cr4: .quad 0
.Lold_xcr0: .quad 0
.Lmpx_expected: .quad 0
.Lmpx_phase: .long 0
.Lnmi_seen: .long 0
.Lin_nmi: .long 0
.Lirq_seen: .long 0
.Luser_stage: .long 0
.Luser_bench: .long 0
.Lremaining: .long 0
.Lreturned_flags: .quad 0
.Lcase: .long 0
.Lvector: .quad 0
.Lerror: .quad 0
.Lfault_rip: .quad 0
.Lstart: .quad 0
.balign 16
.Lsimd:
    .quad 0x0123456789abcdef,0xfedcba9876543210
    .quad 0x0022446688aaccee,0xffddbb9977553311
    .quad 0x032147658ba9cfed,0xfcdeb89a74563012
    .quad 0x022046648aa8ceec,0xfddfb99b75573113
    .quad 0x052741638dafc9eb,0xfad8be9c72503614
    .quad 0x042640628caec8ea,0xfbd9bf9d73513715
    .quad 0x072543618fadcbe9,0xf8dabc9e70523416
    .quad 0x062442608eaccae8,0xf9dbbd9f71533517
    .quad 0x092b4d6f81a3c5e7,0xf6d4b2907e5c3a18
    .quad 0x082a4c6e80a2c4e6,0xf7d5b3917f5d3b19
    .quad 0x0b294f6d83a1c7e5,0xf4d6b0927c5e381a
    .quad 0x0a284e6c82a0c6e4,0xf5d7b1937d5f391b
    .quad 0x0d2f496b85a7c1e3,0xf2d0b6947a583e1c
    .quad 0x0c2e486a84a6c0e2,0xf3d1b7957b593f1d
    .quad 0x0f2d4b6987a5c3e1,0xf0d2b496785a3c1e
    .quad 0x0e2c4a6886a4c2e0,0xf1d3b597795b3d1f
.Literations_text: .asciz "IRET iterations=0x"
.Lbench_only_text: .asciz "IRET BENCH ONLY (matrix skipped)\n"
.Lint_pass_text: .asciz "IRET INT PASS\n"
.Lmpx_pass_text: .asciz "IRET MPX PASS\n"
.Lmpx_skip_text: .asciz "IRET SKIP MPX (CPUID XSAVE/MPX absent)\n"
.Lbench_text: .asciz "IRET kernel_ticks=0x"
.Luser_bench_text: .asciz "IRET user_roundtrip_ticks=0x"
.Lpass_text: .asciz "IRET PASS\n"
.Lfail_text: .asciz "IRET FAIL case/vector/error/rip:\n"
.code16
.org 0xfff0
    ljmpw $0xf000,$0
.org 0x10000
