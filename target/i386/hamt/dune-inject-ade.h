/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_INJECT_ADE_H_
#define _DUNE_INJECT_ADE_H_

struct dune_kvm_cpu;
struct kvm_regs;

typedef enum DuneInjectAdeResult {
    DUNE_INJECT_ADE_NOT_APPLICABLE,
    DUNE_INJECT_ADE_RETRY,
    DUNE_INJECT_ADE_FAILED,
} DuneInjectAdeResult;

DuneInjectAdeResult dune_inject_ade_from_mmio(struct dune_kvm_cpu *vcpu,
                                               struct kvm_regs *regs);
DuneInjectAdeResult dune_inject_ade_from_internal_error(
        struct dune_kvm_cpu *vcpu, struct kvm_regs *regs);

#endif
