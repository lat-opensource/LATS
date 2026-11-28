/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <signal.h>

#include <sys/time.h>
#include <sys/resource.h>
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "loongarch-extcontext.h"

#include "kvm-csr.h"

#include "latx-hamt.h"

#include "dune-arch.h"
#include "dune-inject-ade.h"
#include "dune-info.h"
#include "dune-kvm.h"
#include "dune-kvm-init.h"

#include "hamt-options.h"
#include "hamt-dmw.h"
#include "hamt-fake-fork.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "hamt.h"
#include "hamt-data.h"
#include "latx-options.h"
#include "qemu/error-report.h"
#include "tcg/tcg.h"

#ifdef LOONGARCH_NEW_WORLD
#undef KVM_REG_LOONGARCH
#define KVM_REG_LOONGARCH 0x9000000000000000ULL
#endif

/* dune_loop is called from switch_stack in entry-stack.S */
void dune_loop(struct dune_kvm_cpu *vcpu);

#define __SYSCALL_CLOBBERS \
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8", "memory"

/*
 * KVM and the translated thread share TLS. libc ioctl would overwrite the
 * interrupted thread's errno on EINTR before it resumes inside KVM.
 */
static long dune_kvm_run(int fd)
{
    register long a0 asm("$a0") = fd;
    register long a1 asm("$a1") = KVM_RUN;
    register long a2 asm("$a2") = 0;
    register long a7 asm("$a7") = SYS_ioctl;

    asm volatile("syscall 0" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7)
                 : __SYSCALL_CLOBBERS);
    return a0;
}

static bool dune_do_syscall(struct dune_kvm_cpu *cpu, bool is_fork)
{
    register long int __a7 asm("$a7") = cpu->syscall_parameter[7];
    register long int __a0 asm("$a0") = cpu->syscall_parameter[0];
    register long int __a1 asm("$a1") = cpu->syscall_parameter[1];
    register long int __a2 asm("$a2") = cpu->syscall_parameter[2];
    register long int __a3 asm("$a3") = cpu->syscall_parameter[3];
    register long int __a4 asm("$a4") = cpu->syscall_parameter[4];
    register long int __a5 asm("$a5") = cpu->syscall_parameter[5];
    register long int __a6 asm("$a6") = cpu->syscall_parameter[6];

    __asm__ volatile("syscall    0\n\t"
             : "+r"(__a0)
             : "r"(__a7), "r"(__a1), "r"(__a2), "r"(__a3),
               "r"(__a4), "r"(__a5), "r"(__a6)
             : __SYSCALL_CLOBBERS);

    if (is_fork && __a0 == 0) {
        return true; // child
    }

    cpu->syscall_parameter[0] = __a0;
    return false;
}

extern struct dune_kvm_vm *vm;
// call by child after fork
static struct dune_kvm_cpu *dune_dup_kvm(
        struct dune_kvm_cpu *vcpu, int sysno)
{
    int err = 0;
    int dup_cpuid = 0;
    // 1. create new kvm
    vm = NULL;
    struct dune_kvm_cpu *kvmvcpu = dune_init_kvm_cpu(dup_cpuid, 1);
    if (kvmvcpu == NULL) {
        dune_die_exit("%s:%d dune_init_kvm_cpu\n", __func__, __LINE__);
    }
    kvmvcpu->hamt_mode = vcpu->hamt_mode;
    // 2. set kvm registers
    struct kvm_regs child_regs;
    memcpy(&child_regs, &vcpu->info.regs, sizeof(struct kvm_regs));
    child_regs.gpr[4] = 0; // child always return 0
    kvmvcpu->kvm_run->hypercall.ret = child_regs.gpr[4];
    // if new TLS
    if (vcpu->syscall_parameter[0] & CLONE_SETTLS) {
        child_regs.gpr[2] = vcpu->syscall_parameter[4];
    }
    // if new stack pointer
    if (vcpu->syscall_parameter[1] != 0) {
        child_regs.gpr[3] = vcpu->syscall_parameter[1];
    }
    // return to the next inst of the original syscall
    child_regs.pc = vcpu->info.era + 4;
    // set child GPRs
    err = ioctl(kvmvcpu->vcpu_fd, KVM_SET_REGS, &child_regs);
    if (err < 0) {
        dune_die_exit("%s:%d set child regs (err %d)\n",
                       __func__, __LINE__, err);
    }
    // set child FPU regs
    dune_kvm_enable_fpu(kvmvcpu);
    dune_kvm_set_fpu_regs(kvmvcpu, &(vcpu->info.fpu));
    // set child ebase
    kvmvcpu->info.ebase = vcpu->info.ebase;
    // init child CSR
    dune_kvm_init_csr(kvmvcpu, kvmvcpu->hamt_mode, dup_cpuid);
    // 3. return child kvm vcpu
    return kvmvcpu;
}

#define DUNE_CT_NULL                0
#define DUNE_CT_SAME_VM             1
#define DUNE_CT_DIFF_VM_NEW_STACK   2
#define DUNE_CT_DIFF_VM_OLD_STACK   3

static int dune_get_clone_type(struct dune_kvm_cpu *vcpu, int sysno)
{
    assert(sysno == SYS_CLONE);
//    for (int i = 0; i < 8; ++i) {
//        printf("%s:%d sysno arg[%d] %lx\n", __func__, __LINE__,
//                i, vcpu->syscall_parameter[i]);
//    }

    // clone flags
    if (vcpu->syscall_parameter[0] & CLONE_VM) {
        return DUNE_CT_SAME_VM;
    }

    // clone stack
    if (vcpu->syscall_parameter[1] != 0) {
        return DUNE_CT_DIFF_VM_NEW_STACK;
    }

    return DUNE_CT_DIFF_VM_OLD_STACK;
}

static struct dune_kvm_cpu *dune_emulate_diff_vm_old_stack(
        struct dune_kvm_cpu *vcpu, int sysno)
{
    if (dune_do_syscall(vcpu, true)) {
        return dune_dup_kvm(vcpu, sysno); // child
    }
    return NULL; // parent
}

static struct dune_kvm_cpu *dune_emulate_fork(
        struct dune_kvm_cpu *vcpu, int sysno)
{
    int ct = dune_get_clone_type(vcpu, sysno);
    switch (ct) {
    case DUNE_CT_SAME_VM:
        dune_die("%s:%d DUNE_CT_SAME_VM\n", __func__, __LINE__);
        break;
    case DUNE_CT_DIFF_VM_NEW_STACK:
        dune_die("%s:%d DUNE_CT_DIFF_VM_NEW_STACK\n", __func__, __LINE__);
        break;
    case DUNE_CT_DIFF_VM_OLD_STACK:
        return dune_emulate_diff_vm_old_stack(vcpu, sysno);
    default:
        dune_die("%s:%d unexpected clone type %d from sysno %d\n",
                  __func__, __LINE__, ct, sysno);
        break;
    }
    return NULL;
}

/*
 * The signal only stops KVM_RUN. Capture and allocation run outside its
 * handler, before the virtual CPU executes another instruction.
 */
static void dune_capture_native_interrupt(struct dune_kvm_cpu *vcpu)
{
#ifdef CONFIG_LATX_HACK_MEM
    CPUState *cpu = current_cpu;
    struct kvm_regs regs;
    uint64_t crmd, asid;
    struct kvm_one_reg reg;

    if (!cpu || !aot_tu_enabled() || !option_aot_load ||
        option_int_opt != IRQ_CHECK_ASYNC_NOTIFY ||
        !cpu_loop_exit_requested(cpu)) {
        return;
    }
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs)) {
        error_report("LLVM asynchronous exit: KVM_GET_REGS failed");
        abort();
    }
    if (!in_code_gen_buffer((void *)DMW_OUT(regs.pc))) {
        return;
    }
    CPUX86State *env = cpu->env_ptr;
    uint64_t *frame = hamt_native_async_frame(env, DMW_OUT(regs.pc),
                                               regs.gpr[3]);
    if (!frame) {
        return;
    }
    reg = (struct kvm_one_reg){ .id = KVM_CSR_CRMD, .addr = (uintptr_t)&crmd };
    if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg)) {
        error_report("LLVM asynchronous exit: cannot read CRMD (id=0x%" PRIx64 "): %s",
                     (uint64_t)reg.id, strerror(errno));
        abort();
    }
    /* Leave a helper's critical section intact; notification retries it. */
    if (!(crmd & 4)) {
        return;
    }
    reg = (struct kvm_one_reg){ .id = KVM_CSR_ASID, .addr = (uintptr_t)&asid };
    if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg)) {
        error_report("LLVM asynchronous exit: cannot read ASID");
        abort();
    }
    memcpy(frame, regs.gpr, sizeof(regs.gpr));
    frame[PT_ERA_INDEX] = regs.pc;
    frame[PT_PRMD_INDEX] = crmd & 7;
    frame[PT_ASID_INDEX] = asid;
    crmd &= ~UINT64_C(4);
    reg = (struct kvm_one_reg){ .id = KVM_CSR_CRMD, .addr = (uintptr_t)&crmd };
    if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg)) {
        error_report("LLVM asynchronous exit: cannot mask capture interrupts");
        abort();
    }
    regs.pc = DMW_IN(hamt_native_async_entry);
    regs.gpr[2] = env->ras_tp.native_tp;
    regs.gpr[3] = (DMW_OUT(regs.gpr[3]) - 16) & ~UINT64_C(15);
    regs.gpr[4] = DMW_IN(frame);
    regs.gpr[5] = (uintptr_t)env;
    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs)) {
        error_report("LLVM asynchronous exit: KVM_SET_REGS failed");
        abort();
    }
#endif
}

void dune_loop(struct dune_kvm_cpu *vcpu)
{
    while (true) {
        hamt_start_running();
        long err = dune_kvm_run(vcpu->vcpu_fd);
        int run_errno = err < 0 ? -err : 0;
        hamt_stop_running();
        /*uint64_t sysno = arch_get_sysno(vcpu);*/
        uint64_t sysno = vcpu->syscall_parameter[7];
        struct kvm_regs regs;

        if (err < 0 && (run_errno != EINTR && run_errno != EAGAIN)) {
            dune_die("KVM_RUN : err=%d\n", err);
        }

        if ((err < 0 && run_errno == EINTR) ||
            (err >= 0 && vcpu->kvm_run->exit_reason == KVM_EXIT_INTR)) {
            dune_capture_native_interrupt(vcpu);
            continue;
        }
        if (err < 0) {
            continue;
        }

        /*
         * A scalar MMIO access translated into a DMW window needs an injected
         * ADE. The injection file screens the opcode and effective address;
         * this branch owns the KVM exit policy and fatal fallback.
         */
        if (vcpu->kvm_run->exit_reason == KVM_EXIT_MMIO) {
            if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0) {
                printf("%s:%d ADE in DMW fail to get kvm regs\n",
                        __func__, __LINE__);
                goto dune_handle_ade_in_dmw_fail;
            }
            if (dune_inject_ade_from_mmio(vcpu, &regs) ==
                DUNE_INJECT_ADE_RETRY) {
                continue;
            }
            goto dune_handle_ade_in_dmw_fail;
        }

        /* The 4.19 KVM rejects vector MMIO emulation with INTERNAL_ERROR.
         * Its read path leaves PC at the instruction and its write failure
         * path rolls PC back, so preserve PC when retrying the vector op. */
        if (vcpu->kvm_run->exit_reason == KVM_EXIT_INTERNAL_ERROR) {
            if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0) {
                printf("%s:%d vector ADE fail to get kvm regs\n",
                        __func__, __LINE__);
                goto dune_handle_ade_in_dmw_fail;
            }
            if (dune_inject_ade_from_internal_error(vcpu, &regs) ==
                DUNE_INJECT_ADE_RETRY) {
                continue;
            }
        }

dune_handle_ade_in_dmw_fail:

        if (vcpu->kvm_run->exit_reason != KVM_EXIT_HYPERCALL) {
            err = ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs);

            latx_dump_hamt_context();

            dune_info(" KVM exit %d", vcpu->kvm_run->exit_reason);
            dune_info("-------- HAMT KVM GPRs -----------------");
            dune_info("----------------------------------------");
            dune_info(" PC %p", (void *)regs.pc);
            dune_info("----------------------------------------");
            dune_info(" x64 | i386 | Loong | Value");
            dune_info("-----|------|-------|-------------------");
            dune_info("     |      | 01 RA | 0x%lx", regs.gpr[1]);
            dune_info("     |      | 02 TP | 0x%lx", regs.gpr[2]);
            dune_info("     |      | 03 SP | 0x%lx", regs.gpr[3]);
            dune_info("-----|------|-------|-------------------");
            dune_info("     |      | 04 A0 | 0x%lx", regs.gpr[4]);
            dune_info("     |      | 05 A1 | 0x%lx", regs.gpr[5]);
            dune_info("     |      | 06 A2 | 0x%lx", regs.gpr[6]);
            dune_info("     |      | 07 A3 | 0x%lx", regs.gpr[7]);
            dune_info(" TMP | STMP | 08 A4 | 0x%lx", regs.gpr[8]);
            dune_info(" TMP | STMP | 09 A5 | 0x%lx", regs.gpr[9]);
            dune_info(" TB  | TB   | 10 A6 | 0x%lx", regs.gpr[10]);
            dune_info(" TMP |      | 11 A7 | 0x%lx", regs.gpr[11]);
            dune_info("-----|------|-------|-------------------");
            dune_info(" R8  | TMP  | 12 T0 | 0x%lx", regs.gpr[12]);
            dune_info(" R9  | TMP  | 13 T1 | 0x%lx", regs.gpr[13]);
            dune_info(" R10 | TMP  | 14 T2 | 0x%lx", regs.gpr[14]);
            dune_info(" R11 | TMP  | 15 T3 | 0x%lx", regs.gpr[15]);
            dune_info(" R12 | TMP  | 16 T4 | 0x%lx", regs.gpr[16]);
            dune_info(" R13 | TMP  | 17 T5 | 0x%lx", regs.gpr[17]);
            dune_info(" R14 | TMP  | 18 T6 | 0x%lx", regs.gpr[18]);
            dune_info(" R15 | TMP  | 19 T7 | 0x%lx", regs.gpr[19]);
            dune_info("-----|------|-------|-------------------");
            dune_info(" TMP |      | 20 T8 | 0x%lx", regs.gpr[20]);
            dune_info(" TMP |      | 21 X  | 0x%lx", regs.gpr[21]);
            dune_info(" TMP |      | 22 FP | 0x%lx", regs.gpr[22]);
            dune_info(" ENV | ENV  | 23 S0 | 0x%lx", regs.gpr[23]);
            dune_info("-----|------|-------|-------------------");
            dune_info(" RAX | EAX  | 24 S1 | 0x%lx", regs.gpr[24]);
            dune_info(" RCX | ECX  | 25 S2 | 0x%lx", regs.gpr[25]);
            dune_info(" RDX | EDX  | 26 S3 | 0x%lx", regs.gpr[26]);
            dune_info(" RBX | EBX  | 27 S4 | 0x%lx", regs.gpr[27]);
            dune_info(" RSP | ESP  | 28 S5 | 0x%lx", regs.gpr[28]);
            dune_info(" RBP | EBP  | 29 S6 | 0x%lx", regs.gpr[29]);
            dune_info(" RSI | ESI  | 30 S7 | 0x%lx", regs.gpr[30]);
            dune_info(" RDI | EDI  | 31 S8 | 0x%lx", regs.gpr[31]);
            dune_info("----------------------------------------");

            dune_info("------ HAMT KVM CSRs -------------------");
            dune_info("----------------------------------------");
            struct kvm_one_reg csr;
            uint64_t csr_value = 0;

#define DUMP_KVM_CSR(csrid, name) do {                  \
    csr.addr = (uint64_t)&(csr_value);                  \
    csr.id = csrid;                                     \
    err = ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &csr);  \
    dune_info("%-12s 0x%lx", name, csr_value);          \
} while (0)

            uint64_t badv;
            DUMP_KVM_CSR(KVM_CSR_ASID,      "ASID");
            DUMP_KVM_CSR(KVM_CSR_BADV,      "BADV");
            DUMP_KVM_CSR(KVM_CSR_BADI,      "BADI");
            DUMP_KVM_CSR(KVM_CSR_EPC,       "EPC");
            DUMP_KVM_CSR(KVM_CSR_ESTAT,     "ESTAT");
            DUMP_KVM_CSR(KVM_CSR_STLBPS,    "STLBPS");
            DUMP_KVM_CSR(KVM_CSR_TLBRBADV,  "TLBR BADV");
            badv = csr_value;
            DUMP_KVM_CSR(KVM_CSR_TLBREPC,   "TLBR EPC");
            DUMP_KVM_CSR(KVM_CSR_TLBREHI,   "TLBR EHI");
            DUMP_KVM_CSR(KVM_CSR_TLBRELO0,  "TLBR ELO0");
            DUMP_KVM_CSR(KVM_CSR_TLBRELO1,  "TLBR ELO1");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH0, "KS0");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH1, "KS1");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH2, "KS2");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH3, "KS3");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH4, "KS4");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH5, "KS5");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH6, "KS6");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH7, "KS7");
            DUMP_KVM_CSR(KVM_CSR_KSCRATCH8, "KS8");
            DUMP_KVM_CSR(KVM_CSR_PWCTL0,    "PWCL");
            DUMP_KVM_CSR(KVM_CSR_PWCTL1,    "PWCH");

            (void)badv;
            /*hamt_pgd_print(env, badv);*/

            dune_info("----------------------------------------");
            while (1) ;

            dune_die("KVM_EXIT_IS_NOT_HYPERCALL vcpu=%d exit_reason=%d",
                vcpu->cpu_id, vcpu->kvm_run->exit_reason);
        }

        if (vcpu->fake_fork) {
            bool took_bql = !qemu_mutex_iothread_locked();

            /* fork's RCU callbacks require a quiescent reader and BQL. */
            assert(get_ptr_rcu_reader()->depth == 0);
            if (took_bql) {
                qemu_mutex_lock_iothread();
            }
            /*
             * We are in host userspace here. The save callback allocates
             * memory, so use libc's fork hooks to repair allocator locks
             * in the child of this multithreaded process.
             */
            pid_t pid = rcu_fork_snapshot();
            int saved_errno = errno;

            if (took_bql) {
                qemu_mutex_unlock_iothread();
            }
            errno = saved_errno;
            if (pid == 0) {
                vcpu->fake_fork_fn(vcpu->fake_fork_data);
                _exit(0);
            }

            vcpu->fake_fork_pid = pid > 0 ? pid : -1;
            vcpu->fake_fork = 0;
            continue;
        }

        if (sysno == SYS_KEXEC_LOAD) {
            dune_die("Unsupported syscall");
        }

        if (sysno == SYS_EXIT) {
            dune_kvm_free_vcpu(vcpu);
        }

        if (sysno == SYS_SET_THREAD_AREA) {
            /*arch_set_thread_area(vcpu);*/
            assert(0);
        }

        // fork, clone, clone3
        if (sysno == SYS_CLONE) {
            // 1. load all vcpu registers
            // 1.1 load GPRs
            err = ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &vcpu->info.regs);
            if (err < 0) {
                dune_die("%s:%d clone : kvm get GPRs\n", __func__, __LINE__);
            }
            // 1.2 load EPC (point to the original syscall)
            struct kvm_one_reg csr;
            uint64_t csr_value = 0;
            csr.addr = (uint64_t)&(csr_value);
            csr.id = KVM_CSR_EPC;
            err = ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &csr);
            if (err < 0) {
                dune_die("%s:%d clone : kvm get EPC\n", __func__, __LINE__);
            }
            vcpu->info.era = csr_value;
            // 1.3 load FPU regs
            err = dune_kvm_get_fpu_regs(vcpu, &(vcpu->info.fpu));
            if (err < 0) {
                dune_die("%s:%d clone: kvm get fpu\n", __func__, __LINE__);
            }
            // 2. emulate fork
            struct dune_kvm_cpu *child_vcpu = dune_emulate_fork(vcpu, sysno);
            // 3. child return new vcpu
            if (child_vcpu) {
                vcpu = child_vcpu;
            }
            continue;
        }

        dune_do_syscall(vcpu, false);
    }
}

void (*hamt_do_fake_fork)(void);

pid_t hamt_fake_fork(void *dunekvmcpu, void *fn, void *data)
{
    if (!hamt_is_running()) {
        dune_die("%s:%d should execute in hamt\n", __func__, __LINE__);
    }

    struct dune_kvm_cpu *vcpu = dunekvmcpu;
    vcpu->fake_fork = 1;
    vcpu->fake_fork_pid = -1;
    vcpu->fake_fork_fn = fn;
    vcpu->fake_fork_data = data;

    hamt_do_fake_fork(); // hypcall

    if (vcpu->fake_fork != 0) {
        dune_die("%s:%d not complete\n", __func__, __LINE__);
    }

    return vcpu->fake_fork_pid;
}
