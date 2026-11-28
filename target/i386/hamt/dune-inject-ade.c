/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include "qemu/osdep.h"

#include "dune-info.h"
#include "dune-inject-ade.h"
#include "dune-kvm.h"

#include "hamt-dmw.h"
#include "hamt-internal.h"

#include "tcg/tcg.h"

void hamt_mark_injected_ade(uint64_t pc, uint64_t rj, uint64_t rk);

#define DUNE_DMW_TOP8  0x90ULL
#define DUNE_ADE_TOP8  0xabULL
#define DUNE_TOP8(v)   (((v) >> 56) & 0xffULL)

typedef struct DuneInjectAdeAccess {
    int type;
    int rd;
    int rj;
    int rk;
    int simm;
    uint64_t addr;
} DuneInjectAdeAccess;

static bool dune_decode_inject_ade_access(uint32_t inst, const uint64_t *gpr,
                                          DuneInjectAdeAccess *access)
{
    int type = hamt_mmio_inst_type(inst);
    int rj = (inst >> 5) & 0x1f;
    int rk = (inst >> 10) & 0x1f;

    if (!type) {
        return false;
    }

    access->type = type;
    access->rd = inst & 0x1f;
    access->rj = rj;
    access->rk = rk;
    access->simm = 0;

    switch (type) {
    case 1:
    case 4:
        access->simm = hamt_simm12(inst);
        access->addr = gpr[rj] + access->simm;
        break;
    case 2:
    case 5:
        access->addr = gpr[rj] + gpr[rk];
        break;
    case 3:
        access->simm = hamt_ptr_offset(inst);
        access->addr = gpr[rj] + access->simm;
        break;
    default:
        return false;
    }

    return true;
}

static bool dune_rewrite_injected_ade(uint32_t inst, uint64_t *gpr,
                                      uint64_t *retry_addr)
{
    DuneInjectAdeAccess access;
    int rj = (inst >> 5) & 0x1f;
    int rk = (inst >> 10) & 0x1f;

    if (!dune_decode_inject_ade_access(inst, gpr, &access)) {
        return false;
    }

    gpr[rj] = (gpr[rj] & ~(0xffULL << 56)) | (DUNE_ADE_TOP8 << 56);
    if ((access.type == 2 || access.type == 5) && rk != rj) {
        gpr[rk] &= ~(0xffULL << 56);
    }

    switch (access.type) {
    case 1:
    case 4:
        *retry_addr = gpr[rj] + hamt_simm12(inst);
        break;
    case 2:
    case 5:
        *retry_addr = gpr[rj] + gpr[rk];
        break;
    case 3:
        *retry_addr = gpr[rj] + hamt_ptr_offset(inst);
        break;
    default:
        return false;
    }

    return DUNE_TOP8(*retry_addr) != DUNE_DMW_TOP8;
}

static bool dune_prepare_vector_internal_ade(
        uint32_t inst, uint64_t *gpr, uint64_t *original_rj,
        uint64_t *original_rk)
{
    DuneInjectAdeAccess access;
    uint64_t retry_addr;

    if (!dune_decode_inject_ade_access(inst, gpr, &access) ||
        (access.type != 4 && access.type != 5) ||
        DUNE_TOP8(access.addr) != DUNE_DMW_TOP8) {
        return false;
    }

    *original_rj = gpr[access.rj];
    *original_rk = gpr[access.rk];
    return dune_rewrite_injected_ade(inst, gpr, &retry_addr);
}

static DuneInjectAdeResult dune_set_injected_ade_regs(
        struct dune_kvm_cpu *vcpu, const struct kvm_regs *regs,
        uint64_t epc, uint64_t original_rj, uint64_t original_rk)
{
    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, regs) < 0) {
        printf("%s:%d ADE retry failed to set kvm regs\n",
               __func__, __LINE__);
        return DUNE_INJECT_ADE_FAILED;
    }

    hamt_mark_injected_ade(epc, original_rj, original_rk);
    return DUNE_INJECT_ADE_RETRY;
}

DuneInjectAdeResult dune_inject_ade_from_mmio(struct dune_kvm_cpu *vcpu,
                                               struct kvm_regs *regs)
{
    DuneInjectAdeAccess access;
    uint64_t original_rj;
    uint64_t original_rk;
    uint64_t retry_addr = 0;
    uint32_t *epc;
    uint32_t inst;

    /* KVM advances PC before returning a write MMIO exit. */
    if (vcpu->kvm_run->mmio.is_write) {
        regs->pc -= 4;
    }

    epc = (uint32_t *)DMW_OUT(regs->pc);
    inst = *epc;
    if (!dune_decode_inject_ade_access(inst, (uint64_t *)regs->gpr, &access)) {
        printf("%s:%d kvm mmio epc %p inst %08x\n",
               __func__, __LINE__, epc, inst);
        return DUNE_INJECT_ADE_NOT_APPLICABLE;
    }

    if (DUNE_TOP8(access.addr) != DUNE_DMW_TOP8) {
        dune_info(" KVM mmio not handled at %p inst %08x", epc, inst);
        return DUNE_INJECT_ADE_NOT_APPLICABLE;
    }

    original_rj = regs->gpr[access.rj];
    original_rk = regs->gpr[access.rk];

#ifdef CONFIG_LATX_DEBUG
    printf("%s:%d ====== ADE %lx type %d inst %08x\n",
           __func__, __LINE__, access.addr, access.type, inst);
    printf("%s:%d ====== rj %llx rk %llx simm %x\n",
           __func__, __LINE__, regs->gpr[access.rj], regs->gpr[access.rk],
           access.simm);
#endif
    if (!dune_rewrite_injected_ade(inst, (uint64_t *)regs->gpr, &retry_addr)) {
        printf("%s:%d ADE retry address %lx still in DMW\n",
               __func__, __LINE__, retry_addr);
        return DUNE_INJECT_ADE_FAILED;
    }

#ifdef CONFIG_LATX_DEBUG
    printf("%s:%d set rj to %llx\n",
           __func__, __LINE__, regs->gpr[access.rj]);
    if (access.type == 2 || access.type == 5) {
        printf("%s:%d set rk to %llx\n",
               __func__, __LINE__, regs->gpr[access.rk]);
    }
#endif

    if (!vcpu->kvm_run->mmio.is_write) {
        /* A scalar load destination can overwrite its retry base or index. */
        if (access.type < 4 &&
            (access.rd == access.rj ||
             (access.type == 2 && access.rd == access.rk))) {
            vcpu->kvm_run->mmio.len = 8;
            *((uint64_t *)&vcpu->kvm_run->mmio.data[0]) =
                    regs->gpr[access.rd];
#ifdef CONFIG_LATX_DEBUG
            printf("%s:%d set KVM mmio read : len %d data %lx\n",
                   __func__, __LINE__, vcpu->kvm_run->mmio.len,
                   *((uint64_t *)&vcpu->kvm_run->mmio.data[0]));
#endif
        }
        /* KVM performs PC+4 after this ioctl. Keep the retry on this op. */
        regs->pc -= 4;
    }

    return dune_set_injected_ade_regs(vcpu, regs, (uint64_t)epc,
                                      original_rj, original_rk);
}

DuneInjectAdeResult dune_inject_ade_from_internal_error(
        struct dune_kvm_cpu *vcpu, struct kvm_regs *regs)
{
    uint32_t *epc = (uint32_t *)DMW_OUT(regs->pc);
    uint64_t original_rj = 0;
    uint64_t original_rk = 0;
    uint32_t inst;

    if (!in_code_gen_buffer(epc)) {
        return DUNE_INJECT_ADE_NOT_APPLICABLE;
    }

    inst = *epc;
    if (!dune_prepare_vector_internal_ade(inst, (uint64_t *)regs->gpr,
                                           &original_rj, &original_rk)) {
        return DUNE_INJECT_ADE_NOT_APPLICABLE;
    }

    return dune_set_injected_ade_regs(vcpu, regs, (uint64_t)epc,
                                      original_rj, original_rk);
}
