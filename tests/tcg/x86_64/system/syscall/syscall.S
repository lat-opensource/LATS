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
#ifndef TRANSITION_ITERATIONS
#define TRANSITION_ITERATIONS 100000
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
    ljmpl $56,$ROM(.Lprotected)
.balign 8
.Lboot_gdt:
    .quad 0,0x00af9b000000ffff,0x00cf93000000ffff,0x00cffb000000ffff
    .quad 0x00cff3000000ffff,0x00affb000000ffff,0x00cff3000000ffff
    .quad 0x00cf9b000000ffff,0x00008900a0000067,0
.Lboot_gdtr:
    .word 79
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
    movl $20,%ecx
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
    ljmpl $8,$ROM(.Llong)
.code64

.macro gate n target dpl=0
    movl $ROM(\target),%eax
    movw %ax,IDT+16*\n
    movw $8,IDT+16*\n+2
    movw $(0x8e01+(\dpl<<13)),IDT+16*\n+4
    shrl $16,%eax
    movw %ax,IDT+16*\n+6
    movq $0,IDT+16*\n+8
.endm
.macro case n
    movl $\n,.Lcase(%rip)
    movl $0,.Lexpect_vector(%rip)
.endm
.macro user target cs=43 ss=35
    movabsq $KSTACK,%rsp
    pushq $\ss
    pushq $USTACK
    pushq $0x247
    pushq $\cs
    pushq $ROM(\target)
    iretq
.endm
.macro setmsr index value
    movl $\index,%ecx
    movabsq $\value,%rax
    movq %rax,%rdx
    shrq $32,%rdx
    wrmsr
.endm
.macro seed
    movq $0x1111,%rax
    movq $0x2222,%rbx
    movq $0x4444,%rdx
    movq $0x5555,%rsi
    movq $0x6666,%rdi
    movq $0x7777,%rbp
    movq $0x8888,%r8
    movq $0x9999,%r9
    movq $0xaaaa,%r10
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
/* Save before any comparison so tests include arithmetic flags and scratch GPRs. */
.macro snapshot
    movq %rax,.Lstate+0(%rip)
    movq %rbx,.Lstate+8(%rip)
    movq %rdx,.Lstate+16(%rip)
    movq %rsi,.Lstate+24(%rip)
    movq %rdi,.Lstate+32(%rip)
    movq %rbp,.Lstate+40(%rip)
    movq %r8,.Lstate+48(%rip)
    movq %r9,.Lstate+56(%rip)
    movq %r10,.Lstate+64(%rip)
    movq %r12,.Lstate+72(%rip)
    movq %r13,.Lstate+80(%rip)
    movq %r14,.Lstate+88(%rip)
    movq %r15,.Lstate+96(%rip)
    movq %rcx,.Lstate+104(%rip)
    movq %r11,.Lstate+112(%rip)
    movq %rsp,.Lstate+120(%rip)
    pushfq
    popq .Lstate+128(%rip)
    movw %cs,.Lstate+136(%rip)
    movw %ss,.Lstate+138(%rip)
    fxsave64 0x46000
.endm
.macro check_common
    cld
    leaq .Lstate(%rip),%rsi
    leaq .Lseed_values(%rip),%rdi
    movl $13,%ecx
    repe cmpsq
    jne .Lassert_fail
    leaq .Lsimd(%rip),%rsi
    movl $(0x46000+160),%edi
    movl $32,%ecx
    repe cmpsq
    jne .Lassert_fail
    movl %fs:0,%eax
    cmpl $0x12345678,%eax
    jne .Lassert_fail
    movl %gs:0,%eax
    cmpl $0x76543210,%eax
    jne .Lassert_fail
.endm
.macro check_saved cs ss stack flags
    cmpw $\cs,.Lstate+136(%rip)
    jne .Lassert_fail
    cmpw $\ss,.Lstate+138(%rip)
    jne .Lassert_fail
    movabsq $\stack,%rax
    cmpq %rax,.Lstate+120(%rip)
    jne .Lassert_fail
    movq .Lstate+128(%rip),%rax
    andl $0x3fd7,%eax
    cmpl $\flags,%eax
    jne .Lassert_fail
.endm
.macro check_compat_seed sep=0
    cmpl $0x1111,%eax
    jne .Lcompat_fail
    cmpl $0x2222,%ebx
    jne .Lcompat_fail
    cmpl $0x5555,%esi
    jne .Lcompat_fail
    cmpl $0x6666,%edi
    jne .Lcompat_fail
    cmpl $0x7777,%ebp
    jne .Lcompat_fail
    .if \sep
    cmpl $ROM(.Lsysexitl_return),%edx
    .else
    cmpl $0x4444,%edx
    .endif
    jne .Lcompat_fail
    fxsave 0x46000
    movl $.Lsimd,%esi
    movl $(0x46000+160),%edi
    movl $32,%ecx
    cld
    repe cmpsl
    jne .Lcompat_fail
    movl %fs:0,%eax
    cmpl $0x12345678,%eax
    jne .Lcompat_fail
    movl %gs:0,%eax
    cmpl $0x76543210,%eax
    jne .Lcompat_fail
.endm
.macro done target
    movq $ROM(\target),.Lcontinuation(%rip)
    xorl %eax,%eax
    int $0x80
    jmp .Lassert_fail
.endm
/* Expected faults return directly to a kernel continuation. The handler checks
 * original privilege, stack, flags, instruction address and preserved GPRs. */
.macro fault_setup vector resume cs=8 ss=16 stack=KSTACK
    movl $\vector,.Lexpect_vector(%rip)
    movq $ROM(\resume),.Lresume(%rip)
    movq $\cs,.Lexpect_cs(%rip)
    movq $\ss,.Lexpect_ss(%rip)
    movabsq $\stack,%rax
    movq %rax,.Lexpect_rsp(%rip)
    movq $0x123456,%rbx
    movq $0x654321,%r12
.endm
.macro fault_insn op
    leaq 1f(%rip),%rax
    movq %rax,.Lexpect_rip(%rip)
    pushfq
    popq .Lexpect_flags(%rip)
1:  \op
    jmp .Lassert_fail
.endm
.Llong:
    movabsq $KSTACK,%rsp
    lgdt .Lgdtr(%rip)
    movabsq $KSTACK,%rax
    movq %rax,TSS+4
    movabsq $(HIGH+0x30000),%rax
    movq %rax,TSS+36
    movw $104,TSS+102
    movl $64,%eax
    ltr %ax
    gate 1 .Ldb
    gate 6 .Lud
    gate 13 .Lgp
    gate 14 .Lpf
    gate 0x80 .Lback 3
    gate 0x81 .Lfail 3
    lidt .Lidtr(%rip)
    movb $0xff,%al
    outb %al,$0x21
    outb %al,$0xa1
    /* No timer interrupts; IF remains observable without a live IRQ source. */
    setmsr 0xc0000080 0xd01
    setmsr 0xc0000081 0x001b000b00000000
    setmsr 0xc0000082 (HIGH+ROM(.Lsyscall_entry))
    setmsr 0xc0000083 (HIGH+ROM(.Lcstar_entry))
    setmsr 0xc0000084 0x600
    setmsr 0xc0000100 0x42000
    setmsr 0xc0000101 0x43000
    movl $0x12345678,0x42000
    movl $0x76543210,0x43000
    movl $0,%eax
    cpuid
    cmpl $0x756e6547,%ebx
    sete .Lintel(%rip)
    setmsr 0x174 8
    setmsr 0x175 KSTACK
    setmsr 0x176 (HIGH+ROM(.Lsysenter_entry))
#ifdef TRANSITION_BENCH_ONLY
    leaq .Lbench_only(%rip),%rsi
    call .Lputs
    jmp .Lbench
#endif
    case 1
    user .Lsyscall_user
.Lsyscall_user:
    seed
    pushq $0x647
    popfq
    movq %rbx,%rbx
    leaq 0(%rsp),%rsp
    syscall
.Lsyscall_return:
    snapshot
    check_saved 43 35 USTACK 0x647
    check_common
    cmpq $ROM(.Lsyscall_return),.Lstate+104(%rip)
    jne .Lassert_fail
    cmpq $0x647,.Lstate+112(%rip)
    jne .Lassert_fail
    cld
    done .Lcase2
.Lsyscall_entry:
    pushfq
    cmpl $0,.Lbench_mode(%rip)
    jne .Lsyscall_bench_handler
    popfq
    snapshot
    check_saved 8 16 USTACK 0x2
    check_common
    cmpq $ROM(.Lsyscall_return),.Lstate+104(%rip)
    jne .Lassert_fail
    cmpq $0x647,.Lstate+112(%rip)
    jne .Lassert_fail
    /* The assertions used RCX/R11 and flags. Restore the complete seed state. */
    seed
    movq $ROM(.Lsyscall_return),%rcx
    movq $0x647,%r11
    sysretq
.Lcase2:
    case 2
    seed
    movabsq $0x1122334400000000,%rcx
    addq $ROM(.Lsysretl_user),%rcx
    movq $0x247,%r11
    movq $USTACK,%rsp
    sysretl
.code32
.Lsysretl_user:
    pushfl
    popl .Lcompat_flags
    cmpl $0x247,.Lcompat_flags
    jne .Lcompat_fail
    check_compat_seed
    cmpl $USTACK,%esp
    jne .Lcompat_fail
    movw %cs,%ax
    cmpw $27,%ax
    jne .Lcompat_fail
    movw %ss,%ax
    cmpw $35,%ax
    jne .Lcompat_fail
    movl %fs:0,%eax
    cmpl $0x12345678,%eax
    jne .Lcompat_fail
    movl %gs:0,%eax
    cmpl $0x76543210,%eax
    jne .Lcompat_fail
    movl $ROM(.Lcase3),.Lcontinuation
    xorl %eax,%eax
    int $0x80
.Lcompat_fail:
    int $0x81
.code64
.Lcase3:
    case 3
    setmsr 0xc0000080 0xd00
    fault_setup 6 .Lcase4
    fault_insn syscall
.Lcase4:
    case 4
    fault_setup 6 .Lcase5
    fault_insn sysretq
.Lcase5:
    case 5
    setmsr 0xc0000080 0xd01
    fault_setup 13 .Lcase6 43 35 USTACK
    user .Luser_sysret
.Luser_sysret:
    fault_insn sysretq
.Lcase6:
    case 6
    setmsr 0x174 0
    fault_setup 13 .Lcase7
    cmpb $0,.Lintel(%rip)
    jne 1f
    movl $6,.Lexpect_vector(%rip)
1:  fault_insn sysenter
.Lcase7:
    case 7
    fault_setup 13 .Lcase8
    cmpb $0,.Lintel(%rip)
    jne 1f
    movl $6,.Lexpect_vector(%rip)
1:  fault_insn sysexitl
.Lcase8:
    case 8
    setmsr 0x174 8
    fault_setup 13 .Lcase9 43 35 USTACK
    cmpb $0,.Lintel(%rip)
    jne 1f
    movl $6,.Lexpect_vector(%rip)
1:  user .Luser_sysexit
.Luser_sysexit:
    fault_insn sysexitq
.Lcase9:
    setmsr 0x174 11
    cmpb $0,.Lintel(%rip)
    je .Lskip_sep
    case 9
    movl $0,.Lsep64(%rip)
    seed
    user .Lsysenter_user32 27 35
.code32
.Lsysenter_user32:
    movl $USTACK,%ecx
    movl $ROM(.Lsysexitl_return),%edx
    movl $0x2222,%ebx
    movl $0x5555,%esi
    movl $0x6666,%edi
    movl $0x7777,%ebp
    pushl $0x647
    popfl
    sysenter
.Lsysexitl_return:
    pushfl
    popl .Lcompat_flags
    cmpl $0x47,.Lcompat_flags
    jne .Lcompat_fail
    check_compat_seed 1
    cmpl $USTACK,%esp
    jne .Lcompat_fail
    movw %cs,%ax
    cmpw $27,%ax
    jne .Lcompat_fail
    movw %ss,%ax
    cmpw $35,%ax
    jne .Lcompat_fail
    movl $ROM(.Lcase10),.Lcontinuation
    xorl %eax,%eax
    int $0x80
.code64
.Lsysenter_entry:
    pushfq
    cmpl $0,.Lbench_mode(%rip)
    jne .Lsysenter_bench_handler
    popfq
    snapshot
    check_saved 8 16 KSTACK 0x447
    cmpq $USTACK,.Lstate+104(%rip)
    jne .Lassert_fail
    cmpl $0,.Lsep64(%rip)
    jne .Lsep64_entry
    cmpq $ROM(.Lsysexitl_return),.Lstate+16(%rip)
    jne .Lassert_fail
    movq $0x4444,.Lstate+16(%rip)
    check_common
    cmpq $0x2222,.Lstate+8(%rip)
    jne .Lassert_fail
    cmpq $0x5555,.Lstate+24(%rip)
    jne .Lassert_fail
    cmpq $0x6666,.Lstate+32(%rip)
    jne .Lassert_fail
    cmpq $0x7777,.Lstate+40(%rip)
    jne .Lassert_fail
    seed
    movl $USTACK,%ecx
    movl $ROM(.Lsysexitl_return),%edx
    pushq $0x47
    popfq
    sysexitl
.Lcase10:
    case 10
    movl $1,.Lsep64(%rip)
    user .Lsysenter_user64
.Lsysenter_user64:
    seed
    movq $USTACK,%rcx
    movq $ROM(.Lsysexitq_return),%rdx
    pushq $0x647
    popfq
    sysenter
.Lsep64_entry:
    cmpq $ROM(.Lsysexitq_return),.Lstate+16(%rip)
    jne .Lassert_fail
    /* RDX is the return PC, not its ordinary seeded value. */
    movq $0x4444,.Lstate+16(%rip)
    check_common
    seed
    movq $USTACK,%rcx
    movq $ROM(.Lsysexitq_return),%rdx
    pushq $0x47
    popfq
    sysexitq
.Lsysexitq_return:
    snapshot
    check_saved 43 51 USTACK 0x47
    cmpq $ROM(.Lsysexitq_return),.Lstate+16(%rip)
    jne .Lassert_fail
    movq $0x4444,.Lstate+16(%rip)
    check_common
    done .Lcase11
.Lskip_sep:
    leaq .Lskip_sep_text(%rip),%rsi
    call .Lputs
.Lcase11:
    case 11
    /* Compatibility SYSCALL uses CSTAR and SYSRETL returns there. */
    movl $0,.Lbench_mode(%rip)
    cmpb $0,.Lintel(%rip)
    je 1f
    fault_setup 6 .Lcase12 27 35 USTACK
    movq $ROM(.Lcstar_insn),.Lexpect_rip(%rip)
    movq $0x247,.Lexpect_flags(%rip)
1:  user .Lcstar_user 27 35
.code32
.Lcstar_user:
    movl $0x2222,%ebx
    cmpb $0,.Lintel
    je 1f
    movl $0x123456,%ebx
1:  movl $0x3333,%ecx
    movl $0x4444,%edx
    pushl $0x247
    popfl
    movl %ebx,%ebx
    leal 0(%esp),%esp
.Lcstar_insn:
    syscall
.Lcstar_return:
    cmpl $0x2222,%ebx
    jne .Lcompat_fail
    cmpl $0x4444,%edx
    jne .Lcompat_fail
    cmpl $ROM(.Lcstar_return),%ecx
    jne .Lcompat_fail
    movl $ROM(.Lcase12),.Lcontinuation
    xorl %eax,%eax
    int $0x80
.code64
.Lcstar_entry:
    pushfq
    cmpl $0,.Lbench_mode(%rip)
    jne .Lsyscall_bench_handler
    popfq
    snapshot
    check_saved 8 16 USTACK 0x2
    cmpq $ROM(.Lcstar_return),.Lstate+104(%rip)
    jne .Lassert_fail
    cmpq $0x247,.Lstate+112(%rip)
    jne .Lassert_fail
    cmpq $0x2222,.Lstate+8(%rip)
    jne .Lassert_fail
    cmpq $0x4444,.Lstate+16(%rip)
    jne .Lassert_fail
    movl $0x2222,%ebx
    movl $0x4444,%edx
    movq $ROM(.Lcstar_return),%rcx
    movq $0x247,%r11
    sysretl
.Lcase12:
    case 12
    movl $0,.Ltf_executed(%rip)
    fault_setup 1 .Lcase13 43 35 USTACK
    movq $ROM(.Ltf_user),.Lexpect_rip(%rip)
    movq $0x102,.Lexpect_flags(%rip)
    movq $ROM(.Ltf_user),%rcx
    movq $0x102,%r11
    movq $USTACK,%rsp
    sysretq
.Ltf_user:
    /* With TF=1, #DB must arrive before this first user instruction. */
    movl $1,.Ltf_executed(%rip)
    cmpl $0,.Lexpect_vector(%rip)
    jne .Lassert_fail
    xorl %eax,%eax
    int $0x80
    jmp .Lassert_fail
.Lcase13:
    case 13
    cmpl $0,.Ltf_executed(%rip)
    jne .Lassert_fail
    movq $ROM(.Lcase14),.Lcontinuation(%rip)
    movq $ROM(.Ltf_user),%rcx
    movq $2,%r11
    movq $USTACK,%rsp
    sysretq
.Lcase14:
    case 14
    cmpl $1,.Ltf_executed(%rip)
    jne .Lassert_fail
    setmsr 0xc0000082 (HIGH+ROM(.Ltf_syscall_entry))
    setmsr 0xc0000084 0x100
    movl $0,.Ltf_source(%rip)
    /* POPF does not trap itself; the very next instruction clears TF. */
    pushq $0x102
    popfq
    syscall
.Ltf_kernel_return:
    jmp .Lassert_fail
.Ltf_syscall_entry:
    pushfq
    popq .Lentry_flags(%rip)
    movw %cs,%ax
    cmpw $8,%ax
    jne .Lassert_fail
    movw %ss,%ax
    cmpw $16,%ax
    jne .Lassert_fail
    cmpl $0,.Ltf_source(%rip)
    jne .Ltf_syscall_from_user
    cmpq $2,.Lentry_flags(%rip)
    jne .Lassert_fail
    cmpq $0x102,%r11
    jne .Lassert_fail
    cmpq $ROM(.Ltf_kernel_return),%rcx
    jne .Lassert_fail
    movabsq $KSTACK,%rax
    cmpq %rax,%rsp
    jne .Lassert_fail
    jmp .Lcase15
.Lcase15:
    case 15
    movl $1,.Ltf_source(%rip)
    user .Ltf_syscall_user
.Ltf_syscall_user:
    pushq $0x302
    popfq
    syscall
.Ltf_user_return:
    jmp .Lassert_fail
.Ltf_syscall_from_user:
    cmpq $0x202,.Lentry_flags(%rip)
    jne .Lassert_fail
    cmpq $0x302,%r11
    jne .Lassert_fail
    cmpq $ROM(.Ltf_user_return),%rcx
    jne .Lassert_fail
    cmpq $USTACK,%rsp
    jne .Lassert_fail
    movabsq $KSTACK,%rsp
    jmp .Lcase16
/* Changing LSTAR A/B/A must affect the next transition even when the same
 * source instruction is translated once and then re-entered. */
.Lcase16:
    case 16
    movl $0,.Lroute_hits(%rip)
    movl $0,.Lroute_source(%rip)
    movl $0,.Lroute_index(%rip)
.Lroute_kernel_repeat:
    leaq .Lroute_targets(%rip),%rax
    movl .Lroute_index(%rip),%esi
    movq (%rax,%rsi,8),%rax
    movl $0xc0000082,%ecx
    movq %rax,%rdx
    shrq $32,%rdx
    wrmsr
    andl $1,%esi
    incl %esi
    movl %esi,.Lroute_expected(%rip)
    leaq .Lroute_kernel_return(%rip),%rax
    movq %rax,.Lroute_return(%rip)
    pushfq
    popq .Lroute_flags(%rip)
    syscall
.Lroute_kernel_return:
    incl .Lroute_index(%rip)
    cmpl $3,.Lroute_index(%rip)
    jb .Lroute_kernel_repeat
    cmpl $3,.Lroute_hits(%rip)
    jne .Lassert_fail
    movl $1,.Lroute_source(%rip)
    movl $0,.Lroute_index(%rip)
.Lroute_user_repeat:
    case 17
    leaq .Lroute_targets(%rip),%rax
    movl .Lroute_index(%rip),%esi
    movq (%rax,%rsi,8),%rax
    movl $0xc0000082,%ecx
    movq %rax,%rdx
    shrq $32,%rdx
    wrmsr
    andl $1,%esi
    incl %esi
    movl %esi,.Lroute_expected(%rip)
    movq $ROM(.Lroute_user_done),.Lcontinuation(%rip)
    user .Lroute_user
.Lroute_user:
    leaq .Lroute_user_return(%rip),%rax
    movq %rax,.Lroute_return(%rip)
    pushfq
    popq .Lroute_flags(%rip)
    syscall
.Lroute_user_return:
    xorl %eax,%eax
    int $0x80
    jmp .Lassert_fail
.Lroute_user_done:
    incl .Lroute_index(%rip)
    cmpl $3,.Lroute_index(%rip)
    jb .Lroute_user_repeat
    cmpl $6,.Lroute_hits(%rip)
    jne .Lassert_fail
    setmsr 0xc0000082 (HIGH+ROM(.Lsyscall_entry))
    setmsr 0xc0000084 0x600
    jmp .Lcase18
.Lroute_a:
    movl $1,%eax
    jmp .Lroute_entry
.Lroute_b:
    movl $2,%eax
.Lroute_entry:
    cmpl .Lroute_expected(%rip),%eax
    jne .Lassert_fail
    cmpq .Lroute_return(%rip),%rcx
    jne .Lassert_fail
    cmpq .Lroute_flags(%rip),%r11
    jne .Lassert_fail
    movw %cs,%ax
    cmpw $8,%ax
    jne .Lassert_fail
    movw %ss,%ax
    cmpw $16,%ax
    jne .Lassert_fail
    incl .Lroute_hits(%rip)
    cmpl $0,.Lroute_source(%rip)
    jne .Lroute_return_user
    movabsq $KSTACK,%rax
    cmpq %rax,%rsp
    jne .Lassert_fail
    jmp *%rcx
.Lroute_return_user:
    cmpq $USTACK,%rsp
    jne .Lassert_fail
    sysretq
.Lcase18:
    case 18
    movl $0,.Ltf_executed(%rip)
    fault_setup 1 .Lcase19 27 35 USTACK
    movq $ROM(.Ltf_compat_user),.Lexpect_rip(%rip)
    movq $0x102,.Lexpect_flags(%rip)
    movq $ROM(.Ltf_compat_user),%rcx
    movq $0x102,%r11
    movq $USTACK,%rsp
    sysretl
.code32
.Ltf_compat_user:
    movl $1,.Ltf_executed
    cmpl $0,.Lexpect_vector
    jne .Lcompat_fail
    xorl %eax,%eax
    int $0x80
    jmp .Lcompat_fail
.code64
.Lcase19:
    case 19
    cmpl $0,.Ltf_executed(%rip)
    jne .Lassert_fail
    movq $ROM(.Ltf_compat_done),.Lcontinuation(%rip)
    movq $ROM(.Ltf_compat_user),%rcx
    movq $2,%r11
    movq $USTACK,%rsp
    sysretl
.Ltf_compat_done:
    cmpl $1,.Ltf_executed(%rip)
    jne .Lassert_fail
    leaq .Laddon_pass_text(%rip),%rsi
    call .Lputs
.Lbench:
    case 100
    leaq .Literations_text(%rip),%rsi
    call .Lputs
    movl $TRANSITION_ITERATIONS,%eax
    call .Lhex
    movl $1,.Lbench_mode(%rip)
    movq $ROM(.Lbench_q_done),.Lcontinuation(%rip)
    call .Lstart_clock
    user .Lbench_q_user
.Lbench_q_user:
    syscall
    decl .Lremaining(%rip)
    jnz .Lbench_q_user
    int $0x80
    jmp .Lassert_fail
.Lsyscall_bench_handler:
    popfq
    cmpl $1,.Lbench_mode(%rip)
    jne 1f
    sysretq
1:  sysretl
.Lbench_finish:
    movabsq $KSTACK,%rsp
    call .Lstop_clock
    jmp *.Lcontinuation(%rip)
.Lbench_q_done:
    leaq .Lq_ticks(%rip),%rsi
    call .Lprint_ticks
    movl $2,.Lbench_mode(%rip)
    movq $ROM(.Lbench_l_done),.Lcontinuation(%rip)
    call .Lstart_clock
    cmpb $0,.Lintel(%rip)
    jne 1f
    user .Lbench_l_user 27 35
1:  user .Lbench_l_int_user 27 35
.code32
.Lbench_l_user:
    syscall
    decl .Lremaining
    jnz .Lbench_l_user
    int $0x80
    jmp .Lcompat_fail
.Lbench_l_int_user:
    int $0x80
.Lbench_l_int_return:
    decl .Lremaining
    jnz .Lbench_l_int_user
    int $0x80
    jmp .Lcompat_fail
.code64
.Lbench_l_done:
    leaq .Ll_ticks(%rip),%rsi
    cmpb $0,.Lintel(%rip)
    je 1f
    leaq .Lint_l_ticks(%rip),%rsi
1:  call .Lprint_ticks
    cmpb $0,.Lintel(%rip)
    je .Lpass
    movl $3,.Lbench_mode(%rip)
    movq $ROM(.Lbench_sep_l_done),.Lcontinuation(%rip)
    call .Lstart_clock
    user .Lbench_sep_l_user 27 35
.code32
.Lbench_sep_l_user:
    movl $USTACK,%ecx
    movl $ROM(.Lbench_sep_l_return),%edx
    sysenter
.Lbench_sep_l_return:
    decl .Lremaining
    jnz .Lbench_sep_l_user
    int $0x80
    jmp .Lcompat_fail
.code64
.Lsysenter_bench_handler:
    popfq
    cmpl $3,.Lbench_mode(%rip)
    jne 1f
    sysexitl
1:  sysexitq
.Lbench_sep_l_done:
    leaq .Lsep_l_ticks(%rip),%rsi
    call .Lprint_ticks
    movl $4,.Lbench_mode(%rip)
    movq $ROM(.Lbench_sep_q_done),.Lcontinuation(%rip)
    call .Lstart_clock
    user .Lbench_sep_q_user
.Lbench_sep_q_user:
    movq $USTACK,%rcx
    movq $ROM(.Lbench_sep_q_return),%rdx
    sysenter
.Lbench_sep_q_return:
    decl .Lremaining(%rip)
    jnz .Lbench_sep_q_user
    int $0x80
    jmp .Lassert_fail
.Lbench_sep_q_done:
    leaq .Lsep_q_ticks(%rip),%rsi
    call .Lprint_ticks
.Lpass:
    leaq .Lpass_text(%rip),%rsi
    call .Lputs
    movl $0x10,%eax
    outl %eax,$0xf4
    jmp .Lhalt
.Lstart_clock:
    movl $TRANSITION_ITERATIONS,.Lremaining(%rip)
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    movq %rax,.Lstart(%rip)
    ret
.Lstop_clock:
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    subq .Lstart(%rip),%rax
    movq %rax,.Lticks(%rip)
    ret
.Lprint_ticks:
    call .Lputs
    movq .Lticks(%rip),%rax
    jmp .Lhex
.Lback:
    cmpl $0,.Lbench_mode(%rip)
    je 1f
    cmpl $2,.Lbench_mode(%rip)
    jne .Lbench_finish
    cmpl $0,.Lremaining(%rip)
    je .Lbench_finish
    movq $USTACK,%rsp
    movq $ROM(.Lbench_l_int_return),%rcx
    movq $0x247,%r11
    sysretl
1:  movl $16,%eax
    movw %ax,%ss
    movabsq $KSTACK,%rsp
    jmp *.Lcontinuation(%rip)
.Lassert_fail:
    int $0x81
    jmp .Lhalt
.Ldb:
    pushq $0
    pushq $1
    jmp .Ltrap
.Lud:
    pushq $0
    pushq $6
    jmp .Ltrap
.Lgp:
    pushq $13
    jmp .Ltrap
.Lpf:
    pushq $14
.Ltrap:
    movq (%rsp),%rax
    movq %rax,.Lvector(%rip)
    movq 8(%rsp),%rax
    movq %rax,.Lerror(%rip)
    movq 16(%rsp),%rax
    movq %rax,.Lfault_rip(%rip)
    movl .Lexpect_vector(%rip),%eax
    testl %eax,%eax
    jz .Lfail
    cmpq (%rsp),%rax
    jne .Lfail
    cmpq $0,8(%rsp)
    jne .Lfail
    movq .Lexpect_rip(%rip),%rax
    cmpq 16(%rsp),%rax
    jne .Lfail
    movq .Lexpect_cs(%rip),%rax
    cmpq 24(%rsp),%rax
    jne .Lfail
    movq .Lexpect_ss(%rip),%rax
    cmpq 48(%rsp),%rax
    jne .Lfail
    movq .Lexpect_rsp(%rip),%rax
    cmpq 40(%rsp),%rax
    jne .Lfail
    movq 32(%rsp),%rax
    andq $~0x10000,%rax
    cmpq .Lexpect_flags(%rip),%rax
    jne .Lfail
    cmpq $0x123456,%rbx
    jne .Lfail
    cmpq $0x654321,%r12
    jne .Lfail
    movabsq $KSTACK,%rsp
    movl $16,%eax
    movw %ax,%ss
    pushq $8
    pushq .Lresume(%rip)
    lretq
.Lfail:
    cld
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
1:  pause
    jmp 1b
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
    .word 79
    .quad HIGH+GDT
.Lidtr:
    .word 16*130-1
    .quad IDT
.Lseed_values:
    .quad 0x1111,0x2222,0x4444,0x5555,0x6666,0x7777,0x8888,0x9999,0xaaaa
    .quad 0xcccc,0xdddd,0xeeee,0xffff
.balign 16
.Lsimd:
    .quad 81985529216486895,18364758544493064720
    .quad 81985529216486896,18364758544493064719
    .quad 81985529216486897,18364758544493064718
    .quad 81985529216486898,18364758544493064717
    .quad 81985529216486899,18364758544493064716
    .quad 81985529216486900,18364758544493064715
    .quad 81985529216486901,18364758544493064714
    .quad 81985529216486902,18364758544493064713
    .quad 81985529216486903,18364758544493064712
    .quad 81985529216486904,18364758544493064711
    .quad 81985529216486905,18364758544493064710
    .quad 81985529216486906,18364758544493064709
    .quad 81985529216486907,18364758544493064708
    .quad 81985529216486908,18364758544493064707
    .quad 81985529216486909,18364758544493064706
    .quad 81985529216486910,18364758544493064705
.Lstate: .zero 144
.Lcase: .long 0
.Lvector: .quad 0
.Lerror: .quad 0
.Lfault_rip: .quad 0
.Lexpect_vector: .long 0
.Lexpect_rip: .quad 0
.Lexpect_cs: .quad 0
.Lexpect_ss: .quad 0
.Lexpect_rsp: .quad 0
.Lexpect_flags: .quad 0
.Lresume: .quad 0
.Lcontinuation: .quad 0
.Lcompat_flags: .long 0
.Lsep64: .long 0
.Lintel: .byte 0
.Ltf_executed: .long 0
.Ltf_source: .long 0
.Lentry_flags: .quad 0
.Lroute_hits: .long 0
.Lroute_source: .long 0
.Lroute_index: .long 0
.Lroute_expected: .long 0
.Lroute_return: .quad 0
.Lroute_flags: .quad 0
.Lroute_targets:
    .quad HIGH+ROM(.Lroute_a),HIGH+ROM(.Lroute_b),HIGH+ROM(.Lroute_a)
.Lbench_mode: .long 0
.Lremaining: .long 0
.Lstart: .quad 0
.Lticks: .quad 0
.Lbench_only: .asciz "TRANSITION BENCH ONLY (matrix skipped)\n"
.Literations_text: .asciz "TRANSITION iterations=0x"
.Lskip_sep_text: .asciz "TRANSITION SKIP SYSENTER/SYSEXIT long mode (non-Intel vendor)\n"
.Lq_ticks: .asciz "TRANSITION syscall_sysretq_ticks=0x"
.Lint_l_ticks: .asciz "TRANSITION int80_sysretl_ticks=0x"
.Ll_ticks: .asciz "TRANSITION cstar_sysretl_ticks=0x"
.Lsep_l_ticks: .asciz "TRANSITION sysenter_sysexitl_ticks=0x"
.Lsep_q_ticks: .asciz "TRANSITION sysenter_sysexitq_ticks=0x"
.Laddon_pass_text: .asciz "TRANSITION TF/LSTAR PASS\n"
.Lpass_text: .asciz "TRANSITION PASS\n"
.Lfail_text: .asciz "TRANSITION FAIL case/vector/error/rip:\n"
.code16
.org 0xfff0
    ljmpw $0xf000,$0
.org 0x10000
