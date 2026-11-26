/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Flat 64 KiB PC firmware; no host OS or bootloader dependencies. */
.section .text
.code16
.Lrom:
#define ROM(x) (0x60000 + (x - .Lrom))
#define PML4_A 0x1000
#define PML4_B 0x4000
#define PT_A   0x7000
#define PT_B   0x8000
#define DESC   0xd000
#define IDT    0xe000
#define ALIAS  0x400000
#define PROBE_PTE (PT_B + 8*8)
#define PROBE_VA (ALIAS + 0x8000)
/* Executable pages are in normal RAM, above the BIOS tracking region. */
#define CODE_A 0x109000
#define CODE_B 0x10a000
#define CR3_A  (PML4_A + 1)
#define CR3_B  (PML4_B + 2)
#define NOFLUSH 0x8000000000000000
    cli
    /* Run the paged part from RAM, like an OS kernel, not the ISA ROM hole. */
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
    lgdtl %cs:(.Lgdtr-.Lrom)
    movl %cr0,%eax
    orl $1,%eax
    movl %eax,%cr0
    ljmpl $8,$ROM(.Lprotected)
.balign 8
.Lgdt:
    .quad 0,0x00cf9b000000ffff,0x00cf93000000ffff,0x00af9b000000ffff
#ifdef PCID_LEGACY
    /* Selector 0x20: protected-mode data at base 0x1000, limit 0xff. */
    .quad 0x00409300100000ff
#endif
.Lgdtr:
#ifdef PCID_LEGACY
    .word 39
#else
    .word 31
#endif
    .long ROM(.Lgdt)
.code32
.Lprotected:
    movl $16,%eax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movl $0x18000,%esp
#ifdef PCID_LEGACY
    call .Llegacy
#endif
    movl $0x2003,PML4_A
    movl $0x3003,0x2000
    movl $0x0083,0x3000
    movl $PT_A+3,0x3010
    movl $0x5003,PML4_B
    movl $0x6003,0x5000
    movl $0x0083,0x6000
    movl $PT_B+3,0x6010
    movl $CODE_A+3,PT_A
    movl $CODE_B+3,PT_B
#ifdef PCID_CACHE_SMC
    /* Give the physical code alias its own 4K mapping for SMC tracking. */
    movl $0x23000,%edi
    movl $7,%eax
    movl $512,%ecx
3:  stosl
    movl $0,(%edi)
    addl $4,%edi
    addl $0x1000,%eax
    loop 3b
    movl $0x23003,0x3000
    movl $0x23003,0x6000
#endif
    movl $0x30,%eax
    movl %eax,%cr4
    movl $PML4_A,%eax
    movl %eax,%cr3
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax
    wrmsr
    movl %cr0,%eax
    orl $0x80010000,%eax
    movl %eax,%cr0
    ljmpl $24,$ROM(.Llong)
#ifdef PCID_LEGACY
.macro gate32 n target
    movl $ROM(\target),%eax
    movw %ax,IDT+8*\n
    movw $8,IDT+8*\n+2
    movw $0x8e00,IDT+8*\n+4
    shrl $16,%eax
    movw %ax,IDT+8*\n+6
.endm
.macro expect32 vec
    movl $\vec,%ebx
    movl $ROM(1f),%edi
.endm
.macro caught32
    jmp .Lbad32
1:
.endm
.Llegacy:
    gate32 6 .Lud32
    gate32 13 .Lgp32
    lidtl .Lidtr32-.Lrom+0x60000
    movl $7,%eax
    xorl %ecx,%ecx
    cpuid
    btl $10,%ebx
    jnc .Loff32
    movl $0x20020,%eax
    expect32 13
    movl %eax,%cr4
    caught32
    movl $2,%eax
    movl $7,DESC
    invpcid DESC,%eax
    xorl %eax,%eax
    expect32 13
    invpcid DESC,%eax
    caught32
    movl $32,%eax
    movw %ax,%fs
    movl $0,0x1040
    xorl %eax,%eax
    invpcid %fs:0x40,%eax
    expect32 13
    invpcid %fs:0xf8,%eax
    caught32
    xorw %ax,%ax
    movw %ax,%fs
    expect32 13
    invpcid %fs:0x40,%eax
    caught32
    ret
.Loff32:
    expect32 6
    invpcid DESC,%eax
    caught32
    ret
.Lud32:
    pushl $0
    pushl $6
    jmp .Ltrap32
.Lgp32:
    pushl $13
.Ltrap32:
    cmpl %ebx,(%esp)
    jne .Lbad32
    cmpl $0,4(%esp)
    jne .Lbad32
    addl $8,%esp
    movl %edi,(%esp)
    iretl
.Lbad32:
    movl $0x31,%eax
    outb %al,$0xe9
    outl %eax,$0xf4
    hlt
.Lidtr32:
    .word 8*14-1
    .long IDT
#endif
.code64
.macro gate n target
    movl $ROM(\target),%eax
    movw %ax,IDT+16*\n
    movw $24,IDT+16*\n+2
    movw $0x8e00,IDT+16*\n+4
    shrl $16,%eax
    movw %ax,IDT+16*\n+6
    movq $0,IDT+16*\n+8
.endm
.macro fault vec, ec=0
    movl $\vec,%r13d
    movl $\ec,%r14d
    leaq 1f(%rip),%r15
.endm
.macro checkfault
    jmp .Lfail
1:
.endm
.Llong:
    movl $0,0x21108
#ifdef PCID_CACHE_SMC
    movl $1,0x21108
#endif
#if defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0
    orl $2,0x21108
#endif
    gate 6 .Lud
    gate 12 .Lss
    gate 13 .Lgp
    gate 14 .Lpf
    lidt .Lidtr(%rip)
#ifdef HAMT_PTW_CONTRACT
    call .Lptw_contract
    movl $0x55,%eax
    jmp .Ldone
#endif
#ifdef PCID_CR3_NON_PCIDE
    call .Lcr3_non_pcide
    orl $4,0x21108
#endif
    movl $1,%eax
    cpuid
    btl $17,%ecx
    jnc .Loff
    movl $7,%eax
    xorl %ecx,%ecx
    cpuid
    btl $10,%ebx
    jnc .Lfail
#ifdef PCID_SVM
    call .Lsvm_test
#endif
    movq %cr4,%rbx
    orl $0x20000,%ebx
    movl $PML4_A+8,%eax
    movq %rax,%cr3
    fault 13
    movq %rbx,%cr4
    checkfault
    movl $PML4_A,%eax
    movq %rax,%cr3
    movq %rbx,%cr4
    /* Clearing PCIDE does not permit clearing PAE in IA-32e mode. */
    movq %rbx,%rax
    andl $~0x20020,%eax
    fault 13
    movq %rax,%cr4
    checkfault
    movq %cr4,%rax
    cmpq %rbx,%rax
    jne .Lfail
    call .Lwrap_descriptor
    movl $CR3_A,%eax
    movq %rax,%cr3
    call .Llarge_page
#if defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0
    /* Discard large translations left by the INVLPG test. */
    movq $0,DESC
    movl $2,%eax
    invpcid DESC,%rax
#endif
    movl $0x000011b8,CODE_A
    movl $0x0000c300,CODE_A+4
    movl $0x000022b8,CODE_B
    movl $0x0000c300,CODE_B+4
    movb $0x11,CODE_A+64
    movb $0x22,CODE_B+64
    movl $ALIAS,%esi
    movl $CR3_A,%eax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
    movl $CR3_B,%eax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    movq %cr3,%rdx
    cmpq $CR3_A,%rdx
    jne .Lfail
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
#ifdef PCID_BENCH
    /* Both address spaces use the same instruction and data virtual address. */
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    movq %rax,0x21100
#ifdef PCID_INVLPG
    call .Linvlpg_global
#else
#ifdef PCID_INVPCID_TYPE
    /* Initial code-page protection can evict hardware ASIDs. Allow warmup
     * before requiring 4096 retained switches back to A. */
#if PCID_INVPCID_TYPE == 0
    /* Probe a non-adjacent B page for retention in the target ASID. */
    movq $0x120003,PROBE_PTE
    movl $0,0x2110c
#endif
    movl $4112,%ecx
#else
    movl $4096,%ecx
#endif
.Lswitch_pair:
#if defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0
    /* Clear Accessed without INVLPG: an existing translation may stay hot. */
    andq $~0x20,PROBE_PTE
#endif
#ifdef PCID_INVPCID_TYPE
    /* B is inactive and still maps CODE_B. Remap B while A stays hot. */
    movq $2,DESC
    movq $ALIAS,DESC+8
    movq $CODE_A+3,PT_B
    movl $PCID_INVPCID_TYPE,%eax
    invpcid DESC,%rax
#ifdef PCID_INVPCID_BATCH
    /* More requests for B before it can fill its jump caches again. */
    .rept 3
    invpcid DESC,%rax
    .endr
#endif
#endif
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
#ifdef PCID_INVPCID_TYPE
    cmpb $0x11,64(%rsi)
    jne .Lfail
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
    /* Remap the active B back. A must survive both invalidations. */
    movq $CODE_B+3,PT_B
    movl $PCID_INVPCID_TYPE,%eax
    invpcid DESC,%rax
    /* Unbound PCID 17 shares A's slot but cannot invalidate A. */
    movq $17,DESC
    invpcid DESC,%rax
#endif
    cmpb $0x22,64(%rsi)
    jne .Lfail
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
#if defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0
    /* A page walk here would set Accessed on the unrelated B page. */
    movl PROBE_VA,%edx
    testb $0x20,PROBE_PTE
    jnz 1f
    incl 0x2110c
1:
#endif
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
    cmpb $0x11,64(%rsi)
    jne .Lfail
    decl %ecx
    jnz .Lswitch_pair
#endif
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    subq 0x21100,%rax
    movq %rax,0x21100
#endif
#if defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0
    call .Linvpcid_cross_page
    call .Lsplit_large
#endif
#ifdef PCID_CACHE_SMC
    /* Modify A through the identity alias while B owns the CPU. */
    movl $CR3_B,%eax
    movq %rax,%cr3
    movb $0x33,CODE_A+1
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x33,%eax
    jne .Lfail
    movb $0x11,CODE_A+1
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
#endif
    /* PCIDs 1 and 17 compete for the same slot in the HAMT cache. */
    movabsq $(NOFLUSH | (PML4_B + 17)),%rax
    movq %rax,%cr3
    cmpb $0x22,64(%rsi)
    jne .Lfail
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
    movq $1,DESC
    movq $ALIAS,DESC+8
    movq $CODE_B+3,PT_A
    xorl %eax,%eax
    invpcid DESC,%rax
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
    movq $CODE_B+1,PT_A
    movl $1,%eax
    invpcid DESC,%rax
    fault 14,3
    movb $1,64(%rsi)
    checkfault
    movq PT_A,%r8
    movq PT_B,%r9
    movq $CODE_A+3,PT_A
    movq $CODE_B+3,PT_B
    movl $2,%eax
    invpcid DESC,%rax

    .macro check_type3_page expected
        cmpb $\expected,64(%rsi)
        jne .Lfail
        call *%rsi
        cmpl $\expected,%eax
        jne .Lfail
    .endm
    /* Populate two distinct PCIDs with non-global mappings of the same VA. */
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
    check_type3_page 0x22
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    check_type3_page 0x11

    /* Swap both PTEs without an intervening invalidation. */
    movq $CODE_B+3,PT_A
    movq $CODE_A+3,PT_B
    movl $3,%eax
    invpcid DESC,%rax
    check_type3_page 0x22
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
    check_type3_page 0x11
    .purgem check_type3_page

    movq %r8,PT_A
    movq %r9,PT_B
    movl $2,%eax
    invpcid DESC,%rax
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    movl $4,%eax
    fault 13
    invpcid DESC,%rax
    checkfault
    movq $4096,DESC
    xorl %eax,%eax
    fault 13
    invpcid DESC,%rax
    checkfault
    movq $1,DESC
    /* Four-level paging, with both 48- and 57-bit capable CPU models. */
    movl $7,%eax
    xorl %ecx,%ecx
    cpuid
    movabsq $0x800000000000,%rax
    movq %rax,DESC+8
    xorl %eax,%eax
    btl $16,%ecx
    jnc .Ltarget48
    movl $-1,%r13d             /* Any exception on the legal target fails. */
    invpcid DESC,%rax
    movabsq $0x0100000000000000,%rdx
    movq %rdx,DESC+8
.Ltarget48:
    fault 13
    invpcid DESC,%rax
    checkfault
    movabsq $0x800000000000,%rbp
    fault 12
    invpcid (%rbp),%rax
    checkfault
    fault 6
    .byte 0xf0,0x66,0x0f,0x38,0x82,0x06 /* LOCK INVPCID (%rsi),%rax */
    checkfault
    fault 6
    .byte 0x66,0x0f,0x38,0x82,0xc0 /* reserved register descriptor */
    checkfault
    /* addr32 wraps EIP+disp before adding the full 64-bit GS base. */
    movl $0xc0000101,%ecx
    xorl %eax,%eax
    movl $1,%edx
    wrmsr
    movl $2,%eax
    fault 14
    .byte 0x65,0x67,0x66,0x0f,0x38,0x82,0x05 /* invpcid %gs:disp32(%eip),%rax */
    .long DESC-ROM(.Lafter_gs)
.Lafter_gs:
    checkfault
    movq %cr2,%rdx
    movabsq $(0x100000000+DESC),%rax
    cmpq %rax,%rdx
    jne .Lfail
    xorl %edx,%edx
    xorl %eax,%eax
    wrmsr
    /* First descriptor qword mapped, second qword absent. */
    fault 14
    invpcid 0x1ffff8,%rax
    checkfault
    movq %cr0,%rax
    btrl $31,%eax
    fault 13
    movq %rax,%cr0
    checkfault
    /* A same-root write with bit 63 clear is also an invalidation. */
    movq $CODE_A+3,PT_A
    movl $CR3_A,%eax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
    movq %cr4,%rax
    orl $0x80,%eax
    movq %rax,%cr4
    movq $CODE_A+0x103,PT_A
    call *%rsi
    movq $CODE_A+0x103,PT_B
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
    cmpb $0x11,64(%rsi)
    jne .Lfail
    movq $CODE_B+0x103,PT_A
    movq $CODE_B+0x103,PT_B
    invlpg (%rsi)
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    cmpb $0x22,64(%rsi)
    jne .Lfail
    movq $CODE_A+0x103,PT_A
    movl $2,%eax
    invpcid DESC,%rax
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
#ifdef PCID_SMP
    call .Lshootdown
#endif
    movl $0x30,%eax
    movq %rax,%cr4
    movl $1,%eax
    fault 13
    invpcid DESC,%rax
    checkfault
    movq $0x1000,%rax
    btsq $63,%rax
    fault 13
    movq %rax,%cr3
    checkfault
    movl $13,%r12d
    call .Luser_test
    movl $0x10,%eax
    jmp .Ldone
.Loff:
    call .Llarge_page
    movl $7,%eax
    xorl %ecx,%ecx
    cpuid
    btl $10,%ebx
    jc .Lfail
    xorl %eax,%eax
    fault 6
    invpcid DESC,%rax
    checkfault
    movl $0x20030,%eax
    fault 13
    movq %rax,%cr4
    checkfault
    movl $6,%r12d
    call .Luser_test
    movl $0x11,%eax
    jmp .Ldone
.Lud:
    pushq $0
    pushq $6
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
    cmpq %r13,(%rsp)
    jne .Lfail
    cmpq %r14,8(%rsp)
    jne .Lfail
    addq $16,%rsp
    movq %r15,(%rsp)
    iretq
.Lfail:
    movl $0x30,%eax
.Ldone:
    outb %al,$0xe9
#ifdef PCID_WAIT
    cli
2:  hlt
    jmp 2b
#endif
    outl %eax,$0xf4
    hlt
#ifdef PCID_SVM
/* VMCB offsets from AMD APM vol. 2, Appendix B. L2 shares L1's mapping. */
#define VMCB 0x28000
.Lsvm_test:
    movl $0xc0010117,%ecx        /* VM_HSAVE_PA */
    movl $0x29000,%eax
    xorl %edx,%edx
    wrmsr
    movl $0xc0000080,%ecx
    rdmsr
    btsl $12,%eax               /* EFER.SVME */
    wrmsr
    movl %eax,VMCB+0x4d0
    movl $0x6000,VMCB+0x0c      /* report unexpected L2 #GP/#PF to L1 */
    movl $3,VMCB+0x10           /* intercept VMRUN and VMMCALL */
    movl $4,VMCB+0x14           /* intercept INVPCID */
    movl $1,VMCB+0x58           /* guest ASID */
    movl $0x0a9b0018,VMCB+0x410 /* 64-bit CS */
    movl $-1,VMCB+0x414
    movl $0x0c930010,VMCB+0x420 /* writable SS */
    movl $-1,VMCB+0x424
    movq %cr4,%rax
    movq %rax,VMCB+0x548
    movq %cr3,%rax
    movq %rax,VMCB+0x550
    movq %cr0,%rax
    movq %rax,VMCB+0x558
    movq $2,VMCB+0x570          /* RFLAGS */
    movq $ROM(.Ll2_intercept),VMCB+0x578
    movl $VMCB,%eax
    vmrun
    cmpq $0xa2,VMCB+0x70
    jne .Lfail
    cmpq $0x300000,VMCB+0x78    /* EXITINFO1: unmapped descriptor */
    jne .Lfail
    movabsq $0x100000003,%rax
    cmpq %rax,VMCB+0x80         /* EXITINFO2: full register operand */
    jne .Lfail
    cmpq $ROM(.Ll2_insn),VMCB+0x578
    jne .Lfail
    movl $0,VMCB+0x14
    movq $ROM(.Ll2_execute),VMCB+0x578
    movq $0,DESC
    movl $VMCB,%eax
    vmrun
    cmpq $0x81,VMCB+0x70       /* INVPCID completed up to VMMCALL */
    jne .Lfail
    ret
.Ll2_intercept:
    movabsq $0x100000003,%rcx
.Ll2_insn:
    invpcid 0x300000,%rcx
    vmmcall
.Ll2_execute:
    movl $2,%ecx
    invpcid DESC,%rcx
    vmmcall
#endif
.Lwrap_descriptor:
    /* Compatibility mode, FS.base=0x1000 and a 4-GiB segment limit.
     * The first and last linear pages are mapped, linear 4 GiB is absent. */
    subq $16,%rsp
    sgdt (%rsp)
    lgdt .Lwrap_gdtr(%rip)
    movq $0x26003,0x2018
    movq $0x27003,0x26000+8*511
    movq $0xa003,0x27000+8*511
    movq $0,0xaff0
    movq $0,0xaff8
    movq $0,0
    movq $0,8
    movl $PML4_A,%eax
    movq %rax,%cr3
    pushq $8
    leaq .Lwrap32(%rip),%rax
    pushq %rax
    lretq
.code32
.Lwrap32:
    movw $32,%ax
    movw %ax,%fs
    /* Try the split between qwords and every unaligned split within one. */
    movl $0xffffeff1,%esi
    movl $15,%ecx
    xorl %eax,%eax
.Lwrap_next:
    invpcid %fs:(%esi),%eax
    incl %esi
    loop .Lwrap_next
    movw %ax,%fs
    ljmpl $24,$ROM(.Lwrap_done)
.code64
.Lwrap_done:
    lgdt (%rsp)
    addq $16,%rsp
    movq $0,0x2018
    movl $0xfffff000,%eax
    invlpg (%rax)
    ret
.balign 8
.Lwrap_gdt:
    .quad 0,0x00cf9b000000ffff,0x00cf93000000ffff,0x00af9b000000ffff
    .quad 0x00cf93001000ffff
.Lwrap_gdtr:
    .word 39
    .quad ROM(.Lwrap_gdt)
#ifdef PCID_CR3_NON_PCIDE
#define NON_PCIDE_PAGE_A 0x114000
#define NON_PCIDE_PAGE_B 0x115000
#define NON_PCIDE_ITERATIONS 256
.macro check_non_pcide_page expected
    cmpb $\expected,64(%rsi)
    jne .Lfail
    call *%rsi
    cmpl $\expected,%eax
    jne .Lfail
.endm
.Lcr3_non_pcide:
    movq %cr4,%rax
    testl $0x20000,%eax
    jnz .Lfail
    movq PT_A,%r8
    movq PT_B,%r9
    movl $0x000011b8,NON_PCIDE_PAGE_A
    movw $0xc300,NON_PCIDE_PAGE_A+4
    movb $0x11,NON_PCIDE_PAGE_A+64
    movl $0x000022b8,NON_PCIDE_PAGE_B
    movw $0xc300,NON_PCIDE_PAGE_B+4
    movb $0x22,NON_PCIDE_PAGE_B+64
    movq $NON_PCIDE_PAGE_A+3,PT_A
    movq $NON_PCIDE_PAGE_A+3,PT_B
    movl $ALIAS,%esi
    movl $PML4_B,%edi
    movq %rdi,%cr3
    check_non_pcide_page 0x11
    movl $PML4_A,%edi
    movq %rdi,%cr3
    check_non_pcide_page 0x11
#ifdef PCID_INVLPG
    /* A rotated ASID is active, but it has no PCID slot in pcid_valid. */
    movq $NON_PCIDE_PAGE_B+3,PT_A
    invlpg (%rsi)
    check_non_pcide_page 0x22
    movq $NON_PCIDE_PAGE_A+3,PT_A
    invlpg (%rsi)
    check_non_pcide_page 0x11
#endif
    movl $NON_PCIDE_ITERATIONS,%ecx
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rax,%rdx
    movq %rdx,%r10
.Lcr3_non_pcide_loop:
    /* Warm A still points to the old page until CR3 is rewritten. */
    movq $NON_PCIDE_PAGE_B+3,PT_A
    movq %rdi,%cr3
    check_non_pcide_page 0x22

    movl $PML4_B,%edx
    movq %rdx,%cr3
    check_non_pcide_page 0x11
    movq %rdi,%cr3
    check_non_pcide_page 0x22

    movq $NON_PCIDE_PAGE_A+3,PT_A
    movq %rdi,%cr3
    check_non_pcide_page 0x11
    decl %ecx
    jnz .Lcr3_non_pcide_loop
    lfence
    rdtsc
    shlq $32,%rdx
    orq %rdx,%rax
    subq %r10,%rax
    movq %rax,0x21110
    movl $NON_PCIDE_ITERATIONS*4,0x21118

    movq %r8,PT_A
    movq %r9,PT_B
    movq %rdi,%cr3
    ret
#endif
.Llarge_page:
    /* Cache code in one 4K subpage, invalidate a different one of the same PDE. */
    movq $0x200083,0x3008
    movq $0x83,0x3010
    movl $0x000011b8,CODE_A
    movl $0x0000c300,CODE_A+4
    movl $0x000022b8,CODE_A+0x200000
    movl $0x0000c300,CODE_A+0x200004
    /* The HAMT walker can fill a large data mapping without a code lookup. */
    movq $0x83,0x3210          /* VA 0x8400000 maps physical 0..2 MiB */
    movl $(0x8400000+CODE_A),%esi
    cmpl $0x000011b8,(%rsi)
    jne .Lfail
    movq $0x200083,0x3210
    invlpg (0x8402000)
    cmpl $0x000022b8,(%rsi)
    jne .Lfail
    movq $0,0x3210
    invlpg (0x8402000)

    movl $(ALIAS+CODE_A),%esi
    movl $3,%ecx
.Lwarm_large:
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
    loop .Lwarm_large
    movq $0x200083,0x3010
    invlpg (ALIAS+0x2000)
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
    movq $PT_A+3,0x3010
    invlpg (ALIAS+0x2000)
    movq $0,0x3008
    invlpg (CODE_A+0x200000)
    ret
#if defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0
/* The cached entry PC is on the preceding page. Only the immediate of the
 * MOV and its RET move to a different physical page. */
.Linvpcid_cross_page:
    movb $0xb8,CODE_B+0xfff
    movl $0x33,0x112000
    movb $0xc3,0x112004
    movl $0x44,0x113000
    movb $0xc3,0x113004
    movq $0x112003,PT_B+8
    movl $CR3_B,%eax
    movq %rax,%cr3
    movl $(ALIAS+0xfff),%edi
    call *%rdi
    cmpl $0x33,%eax
    jne .Lfail
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    movq $0x113003,PT_B+8
    movq $2,DESC
    movq $ALIAS+0x1007,DESC+8
    xorl %eax,%eax
    invpcid DESC,%rax
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
    call *%rdi
    cmpl $0x44,%eax
    jne .Lfail
    movq $0,PT_B+8
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    ret

#endif
#if defined(PCID_INVLPG) || \
    (defined(PCID_INVPCID_TYPE) && PCID_INVPCID_TYPE == 0)
/* Split a cached 2M leaf in B. Invalidate a different 4K subpage and
 * check the old instruction and data aliases, first in active B, then in
 * inactive B. Walking the new page tables alone would miss the old 2M leaf. */
.Lsplit_large:
    xorl %ebp,%ebp
1:
#ifdef PCID_INVLPG
    movq $0x183,0x6010
#else
    movq $0x83,0x6010
#endif
    movl $CR3_B,%eax
    movq %rax,%cr3
    movl $(ALIAS+CODE_A),%edi
    cmpb $0x11,64(%rdi)
    jne .Lfail
    call *%rdi
    cmpl $0x11,%eax
    jne .Lfail
    testl %ebp,%ebp
    jz 2f
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
2:
    movq $CODE_B+3,PT_B+8*(CODE_A>>12)
    movq $PT_B+3,0x6010
#ifdef PCID_INVLPG
    invlpg (ALIAS+0x2000)
#else
    movq $2,DESC
    movq $ALIAS+0x2000,DESC+8
    xorl %eax,%eax
    invpcid DESC,%rax
#endif
    testl %ebp,%ebp
    jz 3f
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
3:
    cmpb $0x22,64(%rdi)
    jne .Lfail
    call *%rdi
    cmpl $0x22,%eax
    jne .Lfail
    incl %ebp
    cmpl $2,%ebp
    jne 1b
    movq $0,PT_B+8*(CODE_A>>12)
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    ret
#endif
#ifdef PCID_INVLPG
.Linvlpg_global:
    /* Both PCIDs cache the same global mapping. Only B uses the probe VA. */
    movq %cr4,%r11
    movq %r11,%rax
    orl $0x80,%eax
    movq %rax,%cr4
    movq $CODE_A+0x103,PT_A
    movq $CODE_A+0x103,PT_B
    movq $0x120003,PROBE_PTE
    movl $CR3_B,%eax
    movq %rax,%cr3
    movl PROBE_VA,%eax
    movl $ALIAS,%esi
    call *%rsi
    movl $CODE_B+0x103,%r8d
    movl $CODE_A+0x103,%r9d
    movl $0x22,%r10d
    movl $0,0x2110c
    movl $4112,%ecx
1:
    andq $~0x20,PROBE_PTE
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    call *%rsi
    movq %r8,PT_A
    movq %r8,PT_B
    invlpg 64(%rsi)
    cmpb %r10b,64(%rsi)
    jne .Lfail
    call *%rsi
    cmpl %r10d,%eax
    jne .Lfail
    movabsq $(NOFLUSH | CR3_B),%rax
    movq %rax,%cr3
    cmpb %r10b,64(%rsi)
    jne .Lfail
    call *%rsi
    cmpl %r10d,%eax
    jne .Lfail
    movl PROBE_VA,%eax
    testb $0x20,PROBE_PTE
    jnz 2f
    incl 0x2110c
2:
    xchgq %r8,%r9
    xorl $0x33,%r10d
    decl %ecx
    jnz 1b
    movq $CODE_A+3,PT_A
    movq $CODE_B+3,PT_B
    movq $0,PROBE_PTE
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    invlpg (%rsi)
    call .Lsplit_large
    movq %r11,%cr4
    orl $8,0x21108
    ret
#endif
.Luser_test:
    /* RAM GDT: LTR writes the TSS busy bit. */
    leaq .Lgdt(%rip),%rsi
    movl $0x1c000,%edi
    movl $4,%ecx
    rep movsq
    movabsq $0x00cff3000000ffff,%rax
    stosq
    movabsq $0x00affb000000ffff,%rax
    stosq
    movabsq $0x00008901a0000067,%rax
    stosq
    xorq %rax,%rax
    stosq
    lgdt .Luser_gdtr(%rip)
    movq $0x1b000,0x1a004
    movw $48,%ax
    ltr %ax
    gate 0x80 .Lreturn_from_user
    movb $0xee,IDT+16*0x80+5
    lidt .Luser_idtr(%rip)
    orl $4,PML4_A
    orl $4,0x2000
    orl $4,0x3000
    movl $PML4_A,%eax
    movq %rax,%cr3
    movq %rsp,%rbx
    pushq $0x23
    pushq $0x19000
    pushq $2
    pushq $0x2b
    leaq .Luser(%rip),%rax
    pushq %rax
    iretq
.Luser:
    movq %r12,%r13
    xorq %r14,%r14
    leaq 1f(%rip),%r15
    /* The unmapped descriptor must not supersede #GP/#UD at CPL3. */
    invpcid 0x300000,%rax
    checkfault
    int $0x80
.Lreturn_from_user:
    movq %rbx,%rsp
    ret
.Luser_gdtr:
    .word 63
    .quad 0x1c000
.Luser_idtr:
    .word 4095
    .quad IDT
#ifdef PCID_SMP
/* The AP primes the shared VA in its own TLB, then services a real IPI. */
#define APIC 0xfee00000
#define APSTATE 0x21000
.Lshootdown:
    movq $CODE_A+3,PT_A
    movq $0xb003,0x2018
    movl $0xfee00083,0xbfb8
    movl $CR3_A,%eax
    movq %rax,%cr3
    movq $1,DESC
    movq $ALIAS,DESC+8
    gate 0xf1 .Lipi
    lidt .Lap_idtr(%rip)
    movb $0xea,0x20000
    movw $(.Lap16-.Lrom),0x20001
    movw $0xf000,0x20003
    movl $APIC,%edi
    movl $0x1ff,0xf0(%rdi)
    movl $0x01000000,0x310(%rdi)
    movl $0x4500,0x300(%rdi)
    movl $0x4620,0x300(%rdi)
.Lwait_ready:
    cmpl $1,APSTATE
    jne .Lwait_ready
#ifdef PCID_CACHE_SMC
    /* The AP's A bank is inactive when this physical TB is invalidated. */
    movb $0x33,CODE_A+1
    movl $0x01000000,0x310(%rdi)
    movl $0xf1,0x300(%rdi)
3:  cmpl $2,APSTATE
    jne 3b
    movb $0x11,CODE_A+1
#endif
    movq $CODE_B+3,PT_A
    xorl %eax,%eax
    invpcid DESC,%rax
    movl $0x01000000,0x310(%rdi)
    movl $0xf1,0x300(%rdi)
.Lwait_ack:
#ifdef PCID_CACHE_SMC
    cmpl $3,APSTATE
#else
    cmpl $2,APSTATE
#endif
    jne .Lwait_ack
    ret
.code16
.Lap16:
    cli
    lgdtl %cs:(.Lgdtr-.Lrom)
    movl %cr0,%eax
    orl $1,%eax
    movl %eax,%cr0
    ljmpl $8,$ROM(.Lap32)
.code32
.Lap32:
    movl $16,%eax
    movw %ax,%ds
    movw %ax,%ss
    movl $0x30,%eax
    movl %eax,%cr4
    movl $PML4_A,%eax
    movl %eax,%cr3
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax
    wrmsr
    movl %cr0,%eax
    orl $0x80010000,%eax
    movl %eax,%cr0
    ljmpl $24,$ROM(.Lap64)
.code64
.Lap64:
    movl $0x38000,%esp
    lidt .Lap_idtr(%rip)
    movl $0x20030,%eax
    movq %rax,%cr4
    movl $CR3_A,%eax
    movq %rax,%cr3
    movl $APIC,%edi
    movl $0x1ff,0xf0(%rdi)
    movl $ALIAS,%esi
    call *%rsi
    cmpl $0x11,%eax
    jne .Lfail
#ifdef PCID_CACHE_SMC
    movl $CR3_B,%eax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
#endif
    movl $1,APSTATE
    sti
.Lap_wait:
    hlt
    jmp .Lap_wait
.Lipi:
#ifdef PCID_CACHE_SMC
    cmpl $1,APSTATE
    jne 3f
    movabsq $(NOFLUSH | CR3_A),%rax
    movq %rax,%cr3
    call *%rsi
    cmpl $0x33,%eax
    jne .Lfail
    movl $0,0xb0(%rdi)
    movl $2,APSTATE
    iretq
3:
#endif
    xorl %eax,%eax
    invpcid DESC,%rax
    call *%rsi
    cmpl $0x22,%eax
    jne .Lfail
    movl $0,0xb0(%rdi)
#ifdef PCID_CACHE_SMC
    movl $3,APSTATE
#else
    movl $2,APSTATE
#endif
    iretq
.Lap_idtr:
    .word 4095
    .quad IDT
#endif
#ifdef HAMT_PTW_CONTRACT
#include "ptw-contract.inc"
#endif
.Lidtr:
    .word 16*15-1
    .quad IDT
.code16
.org 0xfff0
    ljmpw $0xf000,$0
.org 0x10000
