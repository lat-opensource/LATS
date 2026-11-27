/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#ifdef CONFIG_SOFTMMU
#include "hw/boards.h"
#endif
#include "lats/aot-atomic.h"
#include "lats/aot-cpu-query.h"
#include "lats/aot-x87.h"
#include "lats/aot-system.h"
#include "latx-native-recover.h"
#include "sys-aot-internal.h"
#include "sys-aot-common.h"
#include "sys-aot-format.h"
#include "qemu/rcu.h"
#include "host/cpuinfo.h"
#include "accel/tcg//tb-hash.h"
/* #include "exec/helper-head.h" */
/* #include "helper.h" */
#include "tcg/tcg.h"
#include "tcg/tcg-ldst.h"
#include "translate.h"
#include "latx-helper.h"
#include <math.h>
#include "latx-static-codes.h"
#include <pthread.h>
#include "include/tcg/tcg-split-tb.h"
#include "latx-options.h"
#include "latx-misc.h"
#include <syscall.h>
#include "include/qemu/cacheinfo.h"
#include "accel/tcg/internal.h"
#include "accel/tcg/tb-context.h"
#include "include/latx-hack.h"
#include "include/exec/ram_addr.h"
#include "include/tcg/insn-start-words.h"
#include "file_ctx.h"
#include "qemu/typedefs.h"
#include "include/tcg/fast-itlb.h"
#include "include/crypto/aes.h"
#include "latx-ras.h"
#include "latx-tu.h"
#include "include/tcg/tcg.h"
#include "tcg/tcg-internal.h"
#include "include/qemu/cacheflush.h"
#include "include/exec/cputlb.h"
#include "include/hamt-ptw.h"
#include "accel/tcg/perf.h"
#include "latx-sys-mmio.h"
#include "include/exec/tb-flush.h"
#include "latx/offline/latx_offline_mode.h"
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_LATX_TU
/* An invalid entry has canonical guest registers. A local branch to the
 * first instruction could instead arrive with LLVM temporaries live. */
static bool aot_native_entry_replaceable(const uint32_t *code, uint32_t bytes,
                                         uint32_t direct_offset)
{
    for (uint32_t off = 0; off < bytes; off += 4) {
        uint32_t word = code[off / 4], op = word & 0xfc000000U;
        int64_t delta;
        if (op == 0x50000000U || op == 0x54000000U) {
            delta = sextract64(((word & 1023U) << 16) | ((word >> 10) & 65535U), 0, 26);
        } else if (op == 0x40000000U || op == 0x44000000U ||
                   op == 0x48000000U) {
            delta = sextract64(((word & 31U) << 16) | ((word >> 10) & 65535U), 0, 21);
        } else if (op >= 0x58000000U && op <= 0x6c000000U) {
            delta = sextract64((word >> 10) & 65535U, 0, 16);
        } else {
            continue;
        }
        int64_t target = (int64_t)off + delta * 4;
        if (target == 0 ||
            (direct_offset && target == (int64_t)direct_offset)) {
            return false;
        }
    }
    return true;
}

#define AOT_DIRECT_RESOLVER_BYTES (24 * sizeof(uint32_t))
#define AOT_DIRECT_GUARD_BYTES (46 * sizeof(uint32_t))
#define AOT_DIRECT_COLD_BYTES (AOT_DIRECT_RESOLVER_BYTES + AOT_DIRECT_GUARD_BYTES)
#define AOT_INDIRECT_COLD_BYTES (64 * sizeof(uint32_t))
#define AOT_LINKED_COLD_BYTES (48 * sizeof(uint32_t))
#define AOT_OSR_STUB_BYTES (10 * sizeof(uint32_t))
#define AOT_CORE_COLD_BYTES 20

#ifdef CONFIG_SOFTMMU
/* Materialize a host or guest address without touching mapped guest GPRs. */
static void aot_call_load64(uint32_t *p, unsigned rd, uint64_t value)
{
    p[0] = 0x14000000U | (((value >> 12) & 0xfffff) << 5) | rd;
    p[1] = 0x03800000U | ((value & 0xfff) << 10) | (rd << 5) | rd;
    p[2] = 0x16000000U | (((value >> 32) & 0xfffff) << 5) | rd;
    p[3] = 0x03000000U | (((value >> 52) & 0xfff) << 10) | (rd << 5) | rd;
}

/* PC-relative call through r1; the static wrapper returns to the next word. */
static bool aot_call_far(uint32_t *p, uintptr_t target)
{
    int64_t delta = (int64_t)target - (int64_t)(uintptr_t)p;
    int64_t off = delta >> 2;
    if (!target || (delta & 3) || off != sextract64(off, 0, 36)) return false;
    p[0] = 0x1e000001U | ((((off + (1 << 15)) >> 16) & 0xfffff) << 5);
    p[1] = 0x4c000021U | ((off & 0xffff) << 10);
    return true;
}

static bool aot_native_call_resolver(uint32_t *stub, uint32_t *slot,
                                     TranslationBlock *source, uint64_t pc,
                                     uint32_t mtcc_id, bool ordinary)
{
    uint32_t old = *slot;
    uint32_t opcode = ordinary ? 0x50000000U : 0x54000000U;
    if ((old & 0xfc000000U) != opcode || !scs_enabled() ||
        !(source->flags & HF_CS64_MASK) || source->cs_base ||
        (source->flags & HF_CPL_MASK) != HF_CPL_MASK) return false;
    int64_t closed = ((intptr_t)stub - (intptr_t)slot) >> 2;
    uintptr_t fallback = GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)&stub[22];
    int64_t fail = delta >> 2;
    if (closed != sextract64(closed, 0, 26) || !fallback || (delta & 3) ||
        fail != sextract64(fail, 0, 36) ||
        !aot_call_far(stub, GET_SC_TABLE(mtcc_id, scs_prologue)) ||
        !aot_call_far(stub + 17, GET_SC_TABLE(mtcc_id, scs_epilogue))) return false;
    /* The full static wrapper preserves GPRs, XMMs, TLS and the current SP. */
    stub[2] = 0x001502e4U; /* or a0, s0, zero */
    stub[3] = 0x03000084U; /* lu52i.d a0, a0, 0: leave DMW */
    aot_call_load64(stub + 4, 5, (uintptr_t)source);
    aot_call_load64(stub + 8, 6, pc);
    aot_call_load64(stub + 12, 1, ordinary ?
        (uintptr_t)latx_helper_aot_ordinary_call :
        (uintptr_t)latx_helper_aot_direct_call);
    stub[16] = 0x4c000021U; /* jirl ra, ra, 0 */
    stub[19] = 0x40000c80U; /* beqz a0, stub[22] */
    stub[20] = 0x03240084U; /* lu52i.d a0, a0, 0x900: enter DMW */
    stub[21] = 0x4c000080U; /* jirl zero, a0, 0 */
    stub[22] = 0x1e000004U | ((((fail + (1 << 15)) >> 16) & 0xfffff) << 5);
    stub[23] = 0x4c000080U | ((fail & 0xffff) << 10);
    flush_idcache_range((uintptr_t)stub, (uintptr_t)stub,
                        AOT_DIRECT_RESOLVER_BYTES);
    *slot = opcode | ((closed >> 16) & 1023) |
            (((uint64_t)closed & 65535) << 10);
    return true;
}

/* The guest CALL is complete, but its softRAS push and BL have not run.
 * Return a replacement for the existing cold tail; publish it only after
 * installing the normal BL resolver. No instruction in the hot body changes. */
static uint32_t aot_native_guard_resolver(uint32_t *stub, uint32_t *slot,
                                         TranslationBlock *source, uint64_t pc,
                                         uint32_t *cold_tail, uint32_t mtcc_id)
{
    size_t reset = offsetof(CPUArchState, ras_tp) +
                   offsetof(LatxRasTPState, reset_pending);
    size_t array = offsetof(CPUArchState, tu_generation_array);
    intptr_t retry = (intptr_t)(slot - 6) - (intptr_t)&stub[42];
    intptr_t enter = (intptr_t)stub - (intptr_t)cold_tail;
    uintptr_t fallback = GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)&stub[44];
    int64_t fail = delta >> 2;
    if ((*cold_tail & 0xfc000000U) != 0x50000000U ||
        (reset & 3) || reset > 32764 || array > 2047 ||
        (retry & 3) || (retry >> 2) != sextract64(retry >> 2, 0, 26) ||
        (enter & 3) || (enter >> 2) != sextract64(enter >> 2, 0, 26) ||
        !fallback || (delta & 3) || fail != sextract64(fail, 0, 36) ||
        !aot_call_far(stub + 8, GET_SC_TABLE(mtcc_id, scs_prologue)) ||
        !aot_call_far(stub + 29, GET_SC_TABLE(mtcc_id, scs_epilogue))) {
        return 0;
    }
    stub[0] = 0x00150065U; /* or a1, sp, zero: preserve the original SP form */
    stub[1] = 0x03240063U; /* enter DMW for private stack accesses */
    stub[2] = 0x02ff8063U; /* addi.d sp, sp, -32 */
    stub[3] = 0x29c00064U; /* st.d a0, sp, 0: guest return PC */
    stub[4] = 0x29c02062U; /* st.d tp, sp, 8: compare only, never restore */
    stub[5] = 0x29c04065U; /* st.d a1, sp, 16: original SP */
    stub[6] = 0x240002e5U | ((reset >> 2) << 10); /* ldptr.w a1, s0, reset */
    stub[7] = 0x440000a0U | ((43 - 7) << 10); /* bnez a1, fail */
    stub[10] = 0x001502e4U; /* or a0, s0, zero */
    stub[11] = 0x03000084U; /* leave DMW for C env */
    aot_call_load64(stub + 12, 5, (uintptr_t)source);
    aot_call_load64(stub + 16, 6, pc);
    aot_call_load64(stub + 20, 7, (uintptr_t)slot);
    aot_call_load64(stub + 24, 1, (uintptr_t)latx_helper_aot_direct_guard);
    stub[28] = 0x4c000021U; /* jirl ra, ra, 0 */
    stub[31] = 0x03240063U; /* SCS returns SP outside DMW */
    stub[32] = 0x40000080U | ((43 - 32) << 10); /* beqz a0, fail */
    stub[33] = 0x28c02065U; /* ld.d a1, sp, 8 */
    stub[34] = 0x5c000045U | ((43 - 34) << 10); /* bne tp, a1, fail */
    stub[35] = stub[6];
    stub[36] = 0x440000a0U | ((43 - 36) << 10); /* bnez a1, fail */
    stub[37] = 0x28c002e5U | (array << 10); /* ld.d a1, s0, array */
    stub[38] = 0x382010a6U; /* ldx.bu a2, a1, a0: validated target id */
    stub[39] = 0x400000c0U | ((43 - 39) << 10); /* beqz a2, fail */
    stub[40] = 0x28c00064U; /* ld.d a0, sp, 0 */
    stub[41] = 0x28c04063U; /* ld.d sp, sp, 16 */
    stub[42] = 0x50000000U | (((retry >> 2) >> 16) & 1023) |
               (((uint64_t)(retry >> 2) & 65535) << 10);
    stub[43] = 0x28c04063U; /* failure also restores the original SP */
    stub[44] = 0x1e000004U | ((((fail + (1 << 15)) >> 16) & 0xfffff) << 5);
    stub[45] = 0x4c000080U | ((fail & 0xffff) << 10);
    flush_idcache_range((uintptr_t)stub, (uintptr_t)stub, AOT_DIRECT_GUARD_BYTES);
    return 0x50000000U | (((enter >> 2) >> 16) & 1023) |
           (((uint64_t)(enter >> 2) & 65535) << 10);
}

static bool aot_native_linked_call_resolver(uint32_t *stub, uint32_t *slot,
                                            TranslationBlock *source,
                                            uint64_t pc, uint32_t mtcc_id)
{
    size_t reset = offsetof(CPUArchState, ras_tp) +
                   offsetof(LatxRasTPState, reset_pending);
    size_t eip = offsetof(CPUArchState, eip);
    int64_t closed = ((intptr_t)stub - (intptr_t)slot) >> 2;
    uintptr_t fallback = GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)&stub[46];
    int64_t fail = delta >> 2;
    if ((*slot & 0xfc000000U) != 0x54000000U || !scs_enabled() ||
        !(source->flags & HF_CS64_MASK) || source->cs_base ||
        (source->flags & HF_CPL_MASK) != HF_CPL_MASK ||
        (reset & 3) || reset > 32764 || eip > 2047 ||
        closed != sextract64(closed, 0, 26) || !fallback || (delta & 3) ||
        fail != sextract64(fail, 0, 36) ||
        !aot_call_far(stub + 9, GET_SC_TABLE(mtcc_id, scs_prologue)) ||
        !aot_call_far(stub + 26, GET_SC_TABLE(mtcc_id, scs_epilogue))) {
        return false;
    }
    stub[0] = 0x00150065U; /* or a1, sp, zero: preserve original SP form */
    stub[1] = 0x03240063U; /* enter DMW for private stack accesses */
    stub[2] = 0x02ff8063U; /* addi.d sp, sp, -32 */
    stub[3] = 0x29c00064U; /* st.d a0, sp, 0: expected guest return PC */
    stub[4] = 0x29c02062U; /* st.d tp, sp, 8: compare only */
    stub[5] = 0x29c04065U; /* st.d a1, sp, 16: original SP */
    stub[6] = 0x29c06061U; /* st.d ra, sp, 24: slot + 4 continuation */
    stub[7] = 0x240002e5U | ((reset >> 2) << 10); /* ldptr.w a1, s0, reset */
    stub[8] = 0x440000a0U | ((40 - 8) << 10); /* bnez a1, fail */
    stub[11] = 0x001502e4U; /* or a0, s0, zero */
    stub[12] = 0x03000084U; /* leave DMW for C env */
    aot_call_load64(stub + 13, 5, (uintptr_t)source);
    aot_call_load64(stub + 17, 6, pc);
    aot_call_load64(stub + 21, 1, (uintptr_t)latx_helper_aot_linked_call);
    stub[25] = 0x4c000021U; /* jirl ra, ra, 0 */
    stub[28] = 0x03240063U; /* SCS returns SP outside DMW */
    stub[29] = 0x40000080U | ((40 - 29) << 10); /* beqz a0, fail */
    stub[30] = 0x28c02065U; /* ld.d a1, sp, 8 */
    stub[31] = 0x5c000045U | ((40 - 31) << 10); /* bne tp, a1, fail */
    stub[32] = stub[7];
    stub[33] = 0x440000a0U | ((40 - 33) << 10); /* bnez a1, fail */
    stub[34] = 0x00150086U; /* or a2, a0, zero */
    stub[35] = 0x032400c6U; /* lu52i.d a2, a2, 0x900: enter DMW */
    stub[36] = 0x28c00064U; /* ld.d a0, sp, 0 */
    stub[37] = 0x28c06061U; /* ld.d ra, sp, 24 */
    stub[38] = 0x28c04063U; /* ld.d sp, sp, 16 */
    stub[39] = 0x4c0000c0U; /* jirl zero, a2, 0 */
    aot_call_load64(stub + 40, 5, pc);
    stub[44] = 0x29c002e5U | (eip << 10); /* st.d a1, s0, eip */
    stub[45] = 0x28c04063U; /* ld.d sp, sp, 16 */
    stub[46] = 0x1e000004U | ((((fail + (1 << 15)) >> 16) & 0xfffff) << 5);
    stub[47] = 0x4c000080U | ((fail & 0xffff) << 10);
    flush_idcache_range((uintptr_t)stub, (uintptr_t)stub,
                        AOT_LINKED_COLD_BYTES);
    *slot = 0x54000000U | ((closed >> 16) & 1023) |
            (((uint64_t)closed & 65535) << 10);
    return true;
}

/* Preserve the LLVM frame on an indirect CALL cache miss or compatibility
 * entry fallback. Direct-native hits execute JIRL directly; misses and
 * non-direct targets share the cold softRAS push before branching to slot. */
static bool aot_native_indirect_resolver(uint32_t *stub, uint32_t *slot,
                                        TranslationBlock *source,
                                        uint32_t *poison, uint32_t mtcc_id)
{
    uint32_t *branch = slot - 1;
    uint32_t patched_branch;
    size_t ras = offsetof(CPUArchState, ras_tp) +
                 offsetof(LatxRasTPState, stack);
    size_t reset = offsetof(CPUArchState, ras_tp) +
                   offsetof(LatxRasTPState, reset_pending);
    size_t array = offsetof(CPUArchState, tu_generation_array);
    size_t eip = offsetof(CPUArchState, eip);
    size_t cflags = offsetof(TranslationBlock, cflags);
    intptr_t cont_delta = (intptr_t)(slot + 1) - (intptr_t)&stub[58];
    int64_t cont_words = cont_delta >> 2;
    intptr_t back = (intptr_t)slot - (intptr_t)&stub[62];
    int64_t back_words = back >> 2;
    uintptr_t fallback = GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    int64_t fail = ((int64_t)fallback - (int64_t)(uintptr_t)&stub[53]) >> 2;
    if (*slot != 0x4c0020c1U || !scs_enabled() || (ras & 3) ||
        ras + 8 > 32764 || (reset & 3) ||
        reset > 32764 || array > 2047 || eip > 2047 || cflags > 2047 ||
        !fallback || fail != sextract64(fail, 0, 36) ||
        (cont_delta & 3) || cont_words != sextract64(cont_words, 0, 20) ||
        (back & 3) || back_words != sextract64(back_words, 0, 26)) {
        return false;
    }
    QEMU_BUILD_BUG_ON(CF_INVALID != (1U << 18));
    {
        int64_t old = sextract64(
            ((*branch & 31U) << 16) | ((*branch >> 10) & 65535U), 0, 21);
        intptr_t delta = (intptr_t)stub - (intptr_t)branch;
        int64_t off = delta >> 2;
        if ((*branch & 0xfc0003e0U) != 0x40000120U || branch + old != poison ||
            (delta & 3) || off != sextract64(off, 0, 21)) {
            return false;
        }
        patched_branch = 0x40000120U | ((off & 65535) << 10) | ((off >> 16) & 31);
    }
    if (!aot_call_far(stub + 15, GET_SC_TABLE(mtcc_id, scs_prologue)) ||
        !aot_call_far(stub + 31, GET_SC_TABLE(mtcc_id, scs_epilogue))) return false;
    /* Compatibility NJC check: non-direct cached targets skip C resolution. */
    stub[0] = 0x400000c0U | ((5 - 0) << 10); /* beqz a2, .Ldo_helper */
    stub[1] = 0x28c020e9U; /* ld.d a5, a3, 8 */
    stub[2] = 0x5c000125U | ((5 - 2) << 10); /* bne a5, a1, .Ldo_helper */
    stub[3] = 0x03000809U; /* lu52i.d a5, zero, 2 */
    stub[4] = 0x58000109U | ((55 - 4) << 10); /* beq a4, a5, .Lsoftras_push */
    /* Private frame: return PC, target PC, top, original SP, result, id, TB. */
    stub[5] = 0x29c002e5U | (eip << 10); /* st.d a1, s0, eip */
    stub[6] = 0x00150066U; /* or a2, sp, zero */
    stub[7] = 0x03240063U;
    stub[8] = 0x02ff0063U; /* addi.d sp, sp, -64 */
    stub[9] = 0x29c00064U;
    stub[10] = 0x29c02065U;
    stub[11] = 0x29c04062U;
    stub[12] = 0x29c06066U;
    stub[13] = 0x240002e5U | ((reset >> 2) << 10);
    stub[14] = 0x440000a0U | ((52 - 14) << 10);
    stub[17] = 0x001502e4U;
    stub[18] = 0x03000084U;
    aot_call_load64(stub + 19, 5, (uintptr_t)source);
    /* The shared NOFP prologue keeps its private MMX frame until epilogue.
     * Access this resolver's original frame above it while C is active. */
    stub[23] = 0x28c00066U | ((8 + LATXS_SCS_MMX_FRAME_SIZE) << 10);
    stub[24] = 0x02c00067U | ((40 + LATXS_SCS_MMX_FRAME_SIZE) << 10);
    aot_call_load64(stub + 25, 1, (uintptr_t)latx_helper_aot_indirect_call);
    stub[29] = 0x4c000021U;
    stub[30] = 0x29c00064U | ((32 + LATXS_SCS_MMX_FRAME_SIZE) << 10);
    stub[33] = 0x03240063U;
    stub[34] = 0x28c08066U; /* ld.d a2, sp, 32 */
    stub[35] = 0x400000c0U | ((52 - 35) << 10);
    stub[36] = 0x28c04065U;
    stub[37] = 0x5c000045U | ((52 - 37) << 10);
    stub[38] = stub[13];
    stub[39] = 0x440000a0U | ((52 - 39) << 10);
    stub[40] = 0x28c0c065U; /* ld.d a1, sp, 48: target TB */
    stub[41] = 0x2a8000a7U | (cflags << 10);
    stub[42] = 0x00d248e7U; /* bstrpick.d a3, a3, 18, 18 */
    stub[43] = 0x440000e0U | ((52 - 43) << 10);
    stub[44] = 0x28c0a065U; /* ld.d a1, sp, 40: target generation */
    stub[45] = 0x400000a0U | ((49 - 45) << 10);
    stub[46] = 0x28c002e7U | (array << 10);
    stub[47] = 0x382014e7U; /* ldx.bu a3, a3, a1 */
    stub[48] = 0x400000e0U | ((52 - 48) << 10);
    stub[49] = 0x28c00064U;
    stub[50] = 0x28c06063U;
    stub[51] = 0x50001000U; /* b stub + 55 (.Lsoftras_push) */
    stub[52] = 0x28c06063U;
    stub[53] = 0x1e000004U | ((((fail + (1 << 15)) >> 16) & 0xfffff) << 5);
    stub[54] = 0x4c000080U | ((fail & 0xffff) << 10);
    stub[55] = 0x03400045U | ((LATX_RAS_TP_SIZE - 1) << 10);
    stub[56] = 0x002ddca7U;
    stub[57] = 0x270000e4U | ((ras >> 2) << 10);
    stub[58] = 0x18000008U | (((uint32_t)cont_words & 0xfffffU) << 5);
    stub[59] = 0x270000e8U | (((ras + 8) >> 2) << 10);
    stub[60] = 0x02c004a2U;
    stub[61] = 0x02ff80c6U; /* addi.d a2, a2, -32 (slot adds +32) */
    stub[62] = 0x50000000U | ((back_words >> 16) & 1023) |
               (((uint64_t)back_words & 65535) << 10);
    stub[63] = 0x03400000U;
    flush_idcache_range((uintptr_t)stub, (uintptr_t)stub, AOT_INDIRECT_COLD_BYTES);
    *branch = patched_branch;
    flush_idcache_range((uintptr_t)branch, (uintptr_t)branch, 4);
    return true;
}
#endif

static bool aot_paired_call_push(const aot_block *block,
        const uint8_t *code, uint64_t code_size)
{
    /* Match the complete seven-word TP push, including its closed repair
     * gate. Registers vary with ordinary allocation, offsets do not. */
    uint32_t push[7];
    if (block->inst_ptr_offset < 12 ||
        (uint64_t)block->inst_ptr_offset + 16 > code_size) return false;
    memcpy(push, code + block->inst_ptr_offset - 12, sizeof(push));
    unsigned idx = push[0] & 31, entry = push[1] & 31;
    unsigned pc = push[2] & 31;
    uint32_t stack = offsetof(CPUX86State, ras_tp.stack);
    int64_t repair_delta = (int64_t)block->ras_tp_cold_offset -
                           block->inst_ptr_offset;
    uint64_t rd = (uint64_t)(repair_delta / 4);
    uint32_t gate = 0x50000000U | ((rd >> 16) & 0x3ff) |
                    ((rd & 0xffff) << 10);
    if (!idx || !entry || !pc || (stack & 3) || repair_delta <= 0 ||
        (repair_delta & 3) ||
        push[0] != (0x03400000U | (511U << 10) | (2U << 5) | idx) ||
        push[1] != (0x002c0000U | (3U << 15) | (23U << 10) |
                    (idx << 5) | entry) ||
        push[2] != (0x27000000U | ((stack / 4) << 10) | (entry << 5) | pc) ||
        push[3] != gate || push[4] != 0x03400000U ||
        push[5] != (0x27000000U | (((stack + 8) / 4) << 10) |
                    (entry << 5) | pc) ||
        push[6] != (0x02c00000U | (idx << 5) | 2)) return false;
    return true;
}

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
extern void lats_register_tu_chunk_hint(uintptr_t tbm_addr, uintptr_t start,
                                        uintptr_t end);
#endif

typedef struct {
    const aot_block *blocks;
    unsigned count;
    const aot_rel *rels;
    uint64_t code_size;
    uint32_t file_generation;
    bool valid_checked;
    bool valid_disjoint;
} LatsMiddleExitCache;

typedef struct {
    uint64_t start;
    uint64_t end;
} LatsRelSpan;

static int lats_rel_span_cmp(const void *a, const void *b)
{
    const LatsRelSpan *sa = a, *sb = b;
    return sa->start < sb->start ? -1 : (sa->start > sb->start ? 1 : 0);
}

static __thread LatsMiddleExitCache lats_middle_cache;

static bool aot_paired_call_middle_exit_fast(const aot_block *blocks,
        unsigned count, unsigned index, const aot_rel *rels,
        uint64_t code_size, uint64_t cold, uint64_t end)
{
    if (lats_middle_cache.blocks != blocks ||
        lats_middle_cache.count != count ||
        lats_middle_cache.rels != rels ||
        lats_middle_cache.code_size != code_size ||
        lats_middle_cache.file_generation != aot_file_generation ||
        !lats_middle_cache.valid_checked) {
        bool ok = count > 0;
        uint64_t prev_end = 0;
        unsigned total_rels = 0;
        for (unsigned i = 0; i < count; i++) {
            const aot_block *b = &blocks[i];
            uint64_t base = b->tc.offset_in_tu;
            uint64_t bytes = (uint64_t)b->ir2_num * 4;
            if (base > code_size || bytes > code_size - base ||
                (i > 0 && base < prev_end) ||
                b->rel_start < -1 ||
                (b->rel_start >= 0 && b->rel_end < b->rel_start)) {
                ok = false;
                break;
            }
            prev_end = base + bytes;
            if (b->rel_start >= 0) {
                uint64_t rel_cnt =
                    (uint64_t)b->rel_end - (uint64_t)b->rel_start + 1;
                if (rel_cnt > INT_MAX ||
                    total_rels > INT_MAX - (unsigned)rel_cnt) {
                    ok = false;
                    break;
                }
                total_rels += (unsigned)rel_cnt;
            }
        }
        if (ok && total_rels > 0) {
            LatsRelSpan *spans = g_try_new(LatsRelSpan, total_rels);
            if (!spans) {
                ok = false;
            } else {
                unsigned n = 0;
                for (unsigned i = 0; i < count && ok; i++) {
                    const aot_block *b = &blocks[i];
                    uint64_t base = b->tc.offset_in_tu;
                    if (b->rel_start < 0) continue;
                    for (int j = b->rel_start; j <= b->rel_end; j++) {
                        const aot_rel *rel = &rels[j];
                        if (rel->tc_offset > code_size - base) {
                            ok = false;
                            break;
                        }
                        uint64_t at = base + rel->tc_offset;
                        uint64_t span = (uint64_t)rel->rel_slots_num * 4;
                        if (span > code_size - at) {
                            ok = false;
                            break;
                        }
                        if (span) {
                            spans[n++] = (LatsRelSpan){
                                .start = at,
                                .end = at + span,
                            };
                        }
                    }
                }
                if (ok && n > 1) {
                    qsort(spans, n, sizeof(*spans), lats_rel_span_cmp);
                    for (unsigned k = 1; k < n; k++) {
                        if (spans[k].start < spans[k - 1].end) {
                            ok = false;
                            break;
                        }
                    }
                }
                g_free(spans);
            }
        }
        lats_middle_cache.blocks = blocks;
        lats_middle_cache.count = count;
        lats_middle_cache.rels = rels;
        lats_middle_cache.code_size = code_size;
        lats_middle_cache.file_generation = aot_file_generation;
        lats_middle_cache.valid_checked = true;
        lats_middle_cache.valid_disjoint = ok;
    }
    if (!lats_middle_cache.valid_disjoint || index >= count) {
        return false;
    }
    const aot_block *self = &blocks[index];
    if (self->rel_start < 0) {
        return false;
    }
    uint64_t self_base = self->tc.offset_in_tu;
    bool load = false, exit = false;
    for (int j = self->rel_start; j <= self->rel_end; j++) {
        const aot_rel *rel = &rels[j];
        if (rel->tc_offset > code_size - self_base) {
            return false;
        }
        uint64_t at = self_base + rel->tc_offset;
        uint64_t span = (uint64_t)rel->rel_slots_num * 4;
        if (span > code_size - at) {
            return false;
        }
        if (!span || at >= end || at + span <= cold) continue;
        if (!load && at == cold && rel->kind == LOAD_TB_ADDR &&
            rel->rel_slots_num == 3 &&
            rel->extra_addend == UINT64_MAX && !rel->next_entry_id) {
            load = true;
        } else if (!exit && at == cold + 12 &&
                   rel->kind == CS_NATIVE_TO_BT_RET_N0 &&
                   rel->rel_slots_num == 1 && !rel->extra_addend &&
                   !rel->next_entry_id) {
            exit = true;
        } else {
            return false;
        }
    }
    if (!load || !exit) {
        return false;
    }
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (blocks[mid].tc.offset_in_tu < cold) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    const aot_block *owner = &blocks[lo - 1];
    uint64_t owner_base = owner->tc.offset_in_tu;
    uint64_t owner_bytes = (uint64_t)owner->ir2_num * 4;
    return owner_base < cold && end - owner_base <= owner_bytes;
}

/* Whole-TU composition keeps each donor's cold tail in place. Prove a
 * middle exit from its exact self-TB/ret_n0 stub and this CALL's relocations;
 * another member's cold stub or a native body must never supply the proof. */
static bool aot_paired_call_middle_exit(const aot_block *blocks, unsigned count,
        unsigned index, const aot_rel *rels, const uint8_t *code,
        uint64_t code_size, uint64_t cold, uint64_t returned)
{
    if ((cold & 3) || cold <= returned || cold > code_size ||
        code_size - cold < 16) return false;
    uint32_t stub[4];
    memcpy(stub, code + cold, sizeof(stub));
    if ((stub[0] & 0xfe00001fU) != 0x1400000aU ||
        (stub[1] & 0xffc003ffU) != 0x0380014aU ||
        (stub[2] & 0xfe00001fU) != 0x1600000aU ||
        (stub[3] & 0xfc000000U) != 0x50000000U) return false;

    bool contained = false, load = false, exit = false;
    uint64_t end = cold + sizeof(stub);
    if (aot_paired_call_middle_exit_fast(blocks, count, index, rels,
                                         code_size, cold, end)) {
        return true;
    } else if (lats_middle_cache.valid_disjoint) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        const aot_block *block = &blocks[i];
        uint64_t base = block->tc.offset_in_tu;
        uint64_t bytes = (uint64_t)block->ir2_num * 4;
        if (base > code_size || bytes > code_size - base ||
            (base >= cold && base < end)) return false;
        if (base <= cold && end - base <= bytes) contained = true;
        if (block->rel_start < 0) continue;
        for (int j = block->rel_start; j <= block->rel_end; j++) {
            const aot_rel *rel = &rels[j];
            if (rel->tc_offset > code_size - base) return false;
            uint64_t at = base + rel->tc_offset;
            uint64_t span = (uint64_t)rel->rel_slots_num * 4;
            if (span > code_size - at) return false;
            if (!span || at >= end || at + span <= cold) continue;
            if (i != index) return false;
            if (!load && at == cold && rel->kind == LOAD_TB_ADDR &&
                rel->rel_slots_num == 3 &&
                rel->extra_addend == UINT64_MAX && !rel->next_entry_id) {
                load = true;
            } else if (!exit && at == cold + 12 &&
                       rel->kind == CS_NATIVE_TO_BT_RET_N0 &&
                       rel->rel_slots_num == 1 && !rel->extra_addend &&
                       !rel->next_entry_id) {
                exit = true;
            } else {
                /* Also rejects duplicate records and overlapping CORE/ENTRY. */
                return false;
            }
        }
    }
    return contained && load && exit;
}

/* Recover the nonserialized HRAS bit only from the complete new layout.
 * Old AOT has its self-TB/ret_n0 exit between the slot and continuation. */
static bool aot_paired_call_slot(const aot_block *blocks, unsigned count,
        unsigned index, const aot_rel *rels, const uint8_t *code,
        uint64_t code_size)
{
    const aot_block *block = &blocks[index];
    if (index + 1 >= count || block->last_ir1_type != IR1_TYPE_CALL ||
        !block->inst_ptr_offset || !block->size ||
        !(block->flags & HF_CS64_MASK) || block->cs_base ||
        (block->flags & HF_CPL_MASK) != HF_CPL_MASK ||
        block->jmp_insn_offset[0] == TB_JMP_OFFSET_INVALID ||
        block->jmp_reset_offset[0] == TB_JMP_OFFSET_INVALID ||
        block->rel_start < 0 || block->link_opt[0] != 1) return false;
    const aot_block *next = &blocks[index + 1];
    unsigned bytes = block->jmp_reset_offset[0] - block->jmp_insn_offset[0];
    uint64_t slot = block->tc.offset_in_tu + block->jmp_insn_offset[0];
    uint64_t returned = block->tc.offset_in_tu + block->jmp_reset_offset[0];
    if ((bytes != 4 && bytes != 8) || (slot & 3) ||
        slot > code_size || code_size - slot < bytes ||
        returned != next->tc.offset_in_tu || !next->icount || !next->ir2_num ||
        block->x86_next_pc != block->pc + block->size ||
        next->pc != block->x86_next_pc || next->flags != block->flags ||
        next->cs_base != block->cs_base || next->cflags != block->cflags) {
        return false;
    }
    if (!aot_paired_call_push(block, code, code_size)) return false;
    uint32_t branch, pad;
    memcpy(&branch, code + slot, 4);
    if ((branch & 0xfc000000U) != 0x50000000U) return false;
    if (bytes == 8) {
        memcpy(&pad, code + slot + 4, 4);
        if (pad != 0x03400000U) return false;
    }
    int64_t displacement = sextract64(((branch & 0x3ff) << 16) |
                                      ((branch >> 10) & 0xffff), 0, 26) * 4;
    int64_t cold = (int64_t)slot + displacement;
    if (cold < 0 || (uint64_t)cold >= code_size) return false;
    if ((uint64_t)cold < blocks[count - 1].tc.offset_in_tu) {
        return aot_paired_call_middle_exit(blocks, count, index, rels, code,
                                           code_size, cold, returned);
    }
    /* Preserve the existing recognition of an unmerged TU's last cold tail. */
    bool owns_exit = false;
    for (int j = block->rel_start; j <= block->rel_end; j++) {
        const aot_rel *rel = &rels[j];
        uint64_t at = block->tc.offset_in_tu + rel->tc_offset;
        if (rel->kind == CS_NATIVE_TO_BT_RET_N0 && rel->rel_slots_num == 1 &&
            rel->extra_addend == 0 && at >= (uint64_t)cold &&
            at + 4 <= code_size) owns_exit = true;
    }
    return owns_exit;
}

/* Mode 2 is a hint selection, not a serialized layout identity. Protect
 * fixed slots in every mode, and remove only ordinary saved r1 hints when
 * loading the same cache in mode 0/1. No LLVM entry or helper is rewritten. */
static bool aot_paired_indirect_call(const aot_block *blocks, unsigned count,
        unsigned index, const aot_rel *rels, const uint8_t *code,
        uint64_t code_size)
{
    const aot_block *block = &blocks[index];
    if (index + 1 >= count || block->last_ir1_type != IR1_TYPE_CALLIN ||
        !block->inst_ptr_offset || block->ir2_num < 1 || !block->size ||
        block->rel_start < 0) return false;
    const aot_block *next = &blocks[index + 1];
    uint64_t end = block->tc.offset_in_tu + (uint64_t)block->ir2_num * 4;
    if (end > code_size || end != next->tc.offset_in_tu ||
        !next->ir2_num || !next->icount ||
        next->pc != block->pc + block->size || next->flags != block->flags ||
        next->cflags != block->cflags || next->cs_base != block->cs_base ||
        !aot_paired_call_push(block, code, code_size)) return false;
    uint32_t last;
    memcpy(&last, code + end - 4, 4);
    if ((last & ~0x3e0U) != 0x4c000001U || !(last & 0x3e0U)) return false;
    for (int j = block->rel_start; j <= block->rel_end; j++) {
        if (rels[j].kind == LOAD_HRAS_CALL_HELPER &&
            rels[j].rel_slots_num == 3) return true;
    }
    return false;
}

/* Only the complete saved CALL/return layout may keep conflicting members
 * local. Recognition is independent of the current HRAS hint mode. */
static bool aot_paired_ordinary_tu(const aot_block *blocks, unsigned count,
        const aot_rel *rels, const uint8_t *code, uint64_t code_size)
{
    for (unsigned i = 0; i < count; i++) {
        if (aot_paired_call_slot(blocks, count, i, rels, code, code_size) ||
            aot_paired_indirect_call(blocks, count, i, rels, code, code_size)) {
            return true;
        }
    }
    return false;
}

#ifdef CONFIG_SOFTMMU
static bool aot_ordinary_code_range(const aot_block *blocks, unsigned count,
        const aot_rel *rels, uint64_t start, uint64_t end)
{
    for (unsigned i = 0; i < count; i++) {
        const aot_block *block = &blocks[i];
        if (block->rel_start < 0) continue;
        for (int j = block->rel_start; j <= block->rel_end; j++) {
            const aot_rel *rel = &rels[j];
            if (rel->kind != AOT_NATIVE_CORE &&
                rel->kind != AOT_NATIVE_ENTRY) continue;
            uint64_t first = block->tc.offset_in_tu + rel->tc_offset;
            uint64_t last = first + (uint64_t)rel->rel_slots_num * 4;
            if (start < last && first < end) return false;
        }
    }
    return true;
}

static void aot_load_ordinary_call_hints(const aot_block *blocks,
        unsigned count, const aot_rel *rels, const uint8_t *original,
        uint8_t *loaded, uint64_t code_size)
{
    if (option_tu_hras == 2) return;
    uint32_t stack = offsetof(CPUX86State, ras_tp.stack);
    for (unsigned i = 0; i < count; i++) {
        const aot_block *block = &blocks[i];
        if (aot_paired_indirect_call(blocks, count, i, rels,
                                     original, code_size)) {
            uint64_t at = block->tc.offset_in_tu +
                          (uint64_t)block->ir2_num * 4 - 4;
            if (aot_ordinary_code_range(blocks, count, rels, at, at + 4)) {
                uint32_t call;
                memcpy(&call, loaded + at, 4);
                call &= ~31U; /* Same target, no r1 CALL hint. */
                memcpy(loaded + at, &call, 4);
            }
        }
        if (block->last_ir1_type != IR1_TYPE_RET) continue;
        uint64_t begin = block->tc.offset_in_tu;
        uint64_t end = begin + (uint64_t)block->ir2_num * 4;
        if (end > code_size) continue;
        for (uint64_t at = begin; at + 24 <= end; at += 4) {
            uint32_t w[6];
            memcpy(w, loaded + at, sizeof(w));
            unsigned entry = w[0] & 31, saved = w[1] & 31;
            unsigned ret_pc = w[2] & 31;
            if (!entry || entry == 1 || entry == 2 || entry == 23 || !saved || saved == entry ||
                w[0] != (0x002c0000U | (3U << 15) | (23U << 10) |
                         (2U << 5) | entry) ||
                w[1] != (0x26000000U | (((stack - 16) / 4) << 10) |
                         (entry << 5) | saved) ||
                w[2] != (0x5c000000U | (4U << 10) |
                         (saved << 5) | ret_pc) ||
                w[3] != (0x26000000U | (((stack - 8) / 4) << 10) |
                         (entry << 5) | 1U) ||
                w[4] != (0x02c00000U | (0xfffU << 10) | (2U << 5) | 2U) ||
                w[5] != 0x4c000020U ||
                !aot_ordinary_code_range(blocks, count, rels, at, at + 24)) {
                continue;
            }
            /* The address temporary is dead after the successful load;
             * reuse it for the target without allocating a scratch register. */
            w[3] = (w[3] & ~31U) | entry;
            w[5] = 0x4c000000U | (entry << 5);
            memcpy(loaded + at, w, sizeof(w));
        }
    }
}

/* Only the unlinked ordinary CALL exit, not its page/IRQ checker. The
 * existing eight-byte jump slot falls through a self-TB load and ret_n0. */
static const aot_rel *aot_ordinary_call_exit(const aot_block *block,
                                           const aot_rel *rels, CPUArchState *env)
{
    if (!option_soft_ras || !option_rastoptp || !scs_enabled() ||
        !can_do_hamt(env) || !hamt_mode_dmw() ||
        option_int_opt != IRQ_CHECK_ASYNC_NOTIFY || !tucp_fast_direct_check() ||
        !tucp_generation_offset() || block->last_ir1_type != IR1_TYPE_CALL ||
        !block->inst_ptr_offset || block->size < 5 ||
        !(block->flags & HF_CS64_MASK) || block->cs_base ||
        (block->flags & HF_CPL_MASK) != HF_CPL_MASK || block->rel_start < 0 ||
        block->jmp_insn_offset[0] == TB_JMP_OFFSET_INVALID ||
        block->jmp_reset_offset[0] != block->jmp_insn_offset[0] + 8) {
        return NULL;
    }
    for (int j = block->rel_start + 1; j <= block->rel_end; j++) {
        const aot_rel *rel = &rels[j], *load = &rels[j - 1];
        if (rel->kind == CS_NATIVE_TO_BT_RET_N0 && rel->rel_slots_num == 1 &&
            load->kind == LOAD_TB_ADDR && load->rel_slots_num == 3 &&
            load->extra_addend == UINT64_MAX && !load->next_entry_id &&
            load->tc_offset == block->jmp_insn_offset[0] + 4 &&
            rel->tc_offset == load->tc_offset + 12 &&
            rel->tc_offset + 4 <= block->ir2_num * 4) {
            return rel;
        }
    }
    return NULL;
}
#endif
#endif

#ifndef CONFIG_SOFTMMU
static void *qemu_ram_addr_to_host(uint64_t addr)
{
    fprintf(stderr, "%s:%d user mode not support aot!\n", __func__, __LINE__);
    exit(-1);
    return NULL;
}
#endif

#if DEBUG_AOT
static uint64_t t1, t2;
static int mis_tb;
uint64_t load_aot_time;
uint64_t recover_time;
static uint64_t cmp_time;
static uint64_t hash_time;
static uint64_t for_time;
static uint64_t read_time;
static uint64_t __attribute__((unused)) load_page_tb_time;
static uint64_t register_page_tb_time;
uint64_t load_page_count;
static uint64_t __attribute__((unused)) aot_gen_code_time;
static uint64_t __attribute__((unused)) insert_time;
static uint64_t __attribute__((unused)) time2;

static uint64_t load_tu_time, load_page_tu_time;

static uint32_t mis_page_count;

int load_core_tb_count, load_user_tb_count;
static int register_tb_count;
#endif

#define AOT_PERF_SCHEMA_VERSION "1.0"
#define AOT_PERF_TICKS_PER_SECOND 100000000.0

typedef struct AotPerfCounters {
    uint64_t mmap_ns_core;
    uint64_t mmap_ns_user;
    uint64_t index_build_ns_core;
    uint64_t index_build_ns_user;
    uint64_t loaded_pages_total;
    uint64_t loaded_tb_core;
    uint64_t loaded_tb_user;
    uint64_t lookup_count;
    uint64_t lookup_hit_count;
    uint64_t lookup_reject_count;
    uint64_t lookup_probe_total;
    uint64_t lookup_probe_max;
    uint64_t page_compare_match_count;
    uint64_t page_compare_reject_count;
    uint64_t runtime_lookup_ns;
    uint64_t register_activate_page_count;
    uint64_t register_activate_tb_count;
    uint64_t register_activate_ns;
} AotPerfCounters;

typedef struct AOTLoadedFileIndex {
    aot_tail *tail;
    page_info_array_t *info_array;
    uint32_t page_num;
    uint32_t hash_size;
    AOTPageIndexEntry *hash_table;
    uint64_t bucket_collision_count;
    uint32_t linear_probe_max;
} AOTLoadedFileIndex;

typedef struct AOTLoadIndexSelection {
    AOTPublishedFile *file;
    void *tb_ptr;
    AOTPageIndexEntry *hash_table;
    uint32_t hash_size;
    uint32_t file_generation;
    uint32_t aot_mode;
    bool use_tu;
} AOTLoadIndexSelection;

static uint32_t next_aot_file_generation;

static uint32_t aot_alloc_file_generation(void)
{
    uint32_t generation;

    do {
        generation = (qatomic_fetch_inc(&next_aot_file_generation) + 1) &
            INT32_MAX;
    } while (generation == 0);
    return generation;
}

static int aot_perf_initialized;
static int aot_perf_enabled_flag;
static int aot_perf_summary_emitted;
static const char *aot_perf_run_id = "unknown";
static const char *aot_perf_scenario_id = "unknown";
static const char *aot_perf_mode = "unknown";
static int aot_perf_repeat_index;
static AotPerfCounters aot_perf_counters;

static __thread tb_page_addr_t *aot_tu_locked_pages;
static __thread int aot_tu_locked_pages_num;

#ifdef CONFIG_LATX_TU
typedef struct AOTTUPageDependency {
    vaddr vp;
    tb_page_addr_t pp;
    int64_t pc_delta;
    void *host_page;
} AOTTUPageDependency;

static __thread AOTTUPageDependency aot_tu_dependencies[AOT_TU_MAX_PAGES];
static __thread int aot_tu_dependencies_num;
/* New load-local allocations need explicit cleanup before cpu_loop_exit;
 * cleanup attributes are not run by the loader's nonlocal flush exit. */
static __thread TranslationBlock **aot_tu_conflicts;
static __thread GArray *aot_tu_conflict_source_pages;
/* Remember one permanent AOT conflict, not missing pages or other transient
 * failures. Recheck its current lookup key before skipping the costly load;
 * an address-space change or invalidation can remove the conflict. */
static __thread struct {
    const aot_tu_head *tu;
    vaddr anchor_page, member_pc;
    uint64_t cs_base;
    uint32_t file_generation, flags, cflags;
} aot_tu_blocked;

static bool aot_tu_still_blocked(CPUState *cpu, const aot_tu_head *tu,
                                vaddr anchor_page, uint64_t cs_base)
{
    if (aot_tu_blocked.tu != tu ||
        aot_tu_blocked.file_generation != aot_file_generation ||
        aot_tu_blocked.anchor_page != anchor_page ||
        aot_tu_blocked.cs_base != cs_base) {
        return false;
    }
    CPUArchState *env = cpu->env_ptr;
    int saved_error_code = env->error_code;
    TranslationBlock *old = tb_htable_lookup_noexit(cpu,
        aot_tu_blocked.member_pc, cs_base, aot_tu_blocked.flags,
        aot_tu_blocked.cflags);
    env->error_code = saved_error_code;
    if (old && (old->bool_flags & IS_AOT_TB)) {
        return true;
    }
    aot_tu_blocked.tu = NULL;
    return false;
}

static void aot_clear_tu_conflicts(void)
{
    g_free(aot_tu_conflicts);
    aot_tu_conflicts = NULL;
    if (aot_tu_conflict_source_pages) {
        g_array_free(aot_tu_conflict_source_pages, true);
        aot_tu_conflict_source_pages = NULL;
    }
}
#endif

static void aot_unlock_load_pages(tb_page_addr_t fallback_page)
{
    if (aot_tu_locked_pages_num) {
        for (int i = aot_tu_locked_pages_num - 1; i >= 0; i--) {
            tb_unlock_page0(aot_tu_locked_pages[i]);
        }
        g_free(aot_tu_locked_pages);
        aot_tu_locked_pages = NULL;
        aot_tu_locked_pages_num = 0;
#ifdef CONFIG_LATX_TU
        aot_tu_dependencies_num = 0;
#endif
    } else if (fallback_page != (tb_page_addr_t)-1) {
        tb_unlock_page0(fallback_page);
    }
}

static const char *aot_perf_kind_name(uint32_t aot_mode)
{
    if (aot_mode == AOT_CORE) {
        return "core";
    }
    if (aot_mode == AOT_USER) {
        return "user";
    }
    if (aot_mode == AOT_TU) {
        return "tu";
    }
    return "unknown";
}

static uint64_t aot_perf_read_u64(uint64_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static void aot_perf_add_u64(uint64_t *value, uint64_t delta)
{
    __sync_fetch_and_add(value, delta);
}

static void aot_perf_max_u64(uint64_t *value, uint64_t candidate)
{
    uint64_t old = aot_perf_read_u64(value);
    while (candidate > old) {
        if (__sync_bool_compare_and_swap(value, old, candidate)) {
            return;
        }
        old = aot_perf_read_u64(value);
    }
}

static double aot_perf_seconds(uint64_t ticks)
{
    return (double)ticks / AOT_PERF_TICKS_PER_SECOND;
}

static uint64_t aot_perf_timestamp_ns(void)
{
    struct timespec ts;
    struct timeval tv;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL +
            (uint64_t)ts.tv_nsec;
    }
    if (gettimeofday(&tv, NULL) == 0) {
        return (uint64_t)tv.tv_sec * 1000000000ULL +
            (uint64_t)tv.tv_usec * 1000ULL;
    }
    return 0;
}

static const char *aot_perf_env_or_unknown(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] ? value : "unknown";
}

static int aot_perf_parse_int_env(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || !value[0]) {
        return fallback;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return fallback;
    }

    return (int)parsed;
}

static void aot_perf_json_string(const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");

    fputc('"', stderr);
    for (; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stderr);
            break;
        case '\\':
            fputs("\\\\", stderr);
            break;
        case '\b':
            fputs("\\b", stderr);
            break;
        case '\f':
            fputs("\\f", stderr);
            break;
        case '\n':
            fputs("\\n", stderr);
            break;
        case '\r':
            fputs("\\r", stderr);
            break;
        case '\t':
            fputs("\\t", stderr);
            break;
        default:
            if (*p < 0x20) {
                fprintf(stderr, "\\u%04x", *p);
            } else {
                fputc(*p, stderr);
            }
            break;
        }
    }
    fputc('"', stderr);
}

static void aot_perf_emit_event_begin(const char *event)
{
    fprintf(stderr, "[AOT-PERF] {\"event\":");
    aot_perf_json_string(event);
    fprintf(stderr, ",\"schema_version\":");
    aot_perf_json_string(AOT_PERF_SCHEMA_VERSION);
    fprintf(stderr, ",\"run_id\":");
    aot_perf_json_string(aot_perf_run_id);
    fprintf(stderr, ",\"scenario_id\":");
    aot_perf_json_string(aot_perf_scenario_id);
    fprintf(stderr, ",\"repeat_index\":%d,\"aot_mode\":",
            aot_perf_repeat_index);
    aot_perf_json_string(aot_perf_mode);
    fprintf(stderr, ",\"pid\":%d,\"timestamp_ns\":%llu",
            (int)getpid(), (unsigned long long)aot_perf_timestamp_ns());
}

void aot_perf_init_from_env(void)
{
    const char *enabled;

    if (aot_perf_initialized) {
        return;
    }

    enabled = getenv("LATS_AOT_PERF");
    aot_perf_enabled_flag = enabled && enabled[0] == '1';
    aot_perf_run_id = aot_perf_env_or_unknown("LATS_AOT_PERF_RUN_ID");
    aot_perf_scenario_id =
        aot_perf_env_or_unknown("LATS_AOT_PERF_SCENARIO_ID");
    aot_perf_mode = aot_perf_env_or_unknown("LATS_AOT_PERF_MODE");
    aot_perf_repeat_index =
        aot_perf_parse_int_env("LATS_AOT_PERF_REPEAT_INDEX", 0);
    aot_perf_initialized = 1;
}

bool aot_perf_enabled(void)
{
    if (!aot_perf_initialized) {
        aot_perf_init_from_env();
    }
    return aot_perf_enabled_flag;
}

uint64_t aot_perf_now_ns(void)
{
#if DEBUG_AOT
    return get_tsc();
#else
    return 0;
#endif
}

void aot_perf_emit_run_context(int aot_enabled, int aot_load_enabled,
        int aot_save_enabled)
{
    if (!aot_perf_enabled()) {
        return;
    }
    aot_perf_emit_event_begin("run_context");
    fprintf(stderr,
            ",\"aot_enabled\":%d,\"aot_load_enabled\":%d,"
            "\"aot_save_enabled\":%d}\n",
            aot_enabled, aot_load_enabled, aot_save_enabled);
}

void aot_perf_record_load_file(uint32_t aot_mode, const char *file_name,
        uint64_t file_size, uint32_t page_count, uint32_t tb_count,
        bool integrity_ok, uint64_t mmap_ns)
{
    if (!aot_perf_enabled()) {
        return;
    }

    if (aot_mode == AOT_CORE) {
        aot_perf_add_u64(&aot_perf_counters.mmap_ns_core, mmap_ns);
        aot_perf_add_u64(&aot_perf_counters.loaded_tb_core, tb_count);
    } else if (aot_mode == AOT_USER) {
        aot_perf_add_u64(&aot_perf_counters.mmap_ns_user, mmap_ns);
        aot_perf_add_u64(&aot_perf_counters.loaded_tb_user, tb_count);
    }
    aot_perf_add_u64(&aot_perf_counters.loaded_pages_total, page_count);

    aot_perf_emit_event_begin("load_file");
    fprintf(stderr, ",\"kind\":");
    aot_perf_json_string(aot_perf_kind_name(aot_mode));
    fprintf(stderr, ",\"file_name\":");
    aot_perf_json_string(file_name ? file_name : "");
    fprintf(stderr,
            ",\"file_size\":%llu,\"page_count\":%u,"
            "\"tb_count\":%u,\"integrity_ok\":%s,"
            "\"mmap_seconds\":%.9f}\n",
            (unsigned long long)file_size, page_count, tb_count,
            integrity_ok ? "true" : "false", aot_perf_seconds(mmap_ns));
}

void aot_perf_record_build_index(uint32_t aot_mode, uint32_t page_count,
        uint32_t hash_size, uint64_t bucket_collision_count,
        uint32_t linear_probe_max, uint64_t build_ns)
{
    if (!aot_perf_enabled()) {
        return;
    }

    if (aot_mode == AOT_CORE) {
        aot_perf_add_u64(&aot_perf_counters.index_build_ns_core, build_ns);
    } else if (aot_mode == AOT_USER) {
        aot_perf_add_u64(&aot_perf_counters.index_build_ns_user, build_ns);
    }

    aot_perf_emit_event_begin("build_index");
    fprintf(stderr, ",\"kind\":");
    aot_perf_json_string(aot_perf_kind_name(aot_mode));
    fprintf(stderr,
            ",\"page_count\":%u,\"hash_size\":%u,"
            "\"bucket_collision_count\":%llu,"
            "\"linear_probe_max\":%u,\"build_seconds\":%.9f}\n",
            page_count, hash_size,
            (unsigned long long)bucket_collision_count, linear_probe_max,
            aot_perf_seconds(build_ns));
}

void aot_perf_record_lookup(bool hit, uint32_t probe_len,
        bool page_hash_equal, bool page_compare_equal, uint64_t lookup_ns)
{
    if (!aot_perf_enabled()) {
        return;
    }

    aot_perf_add_u64(&aot_perf_counters.lookup_count, 1);
    aot_perf_add_u64(&aot_perf_counters.lookup_probe_total, probe_len);
    aot_perf_max_u64(&aot_perf_counters.lookup_probe_max, probe_len);
    aot_perf_add_u64(&aot_perf_counters.runtime_lookup_ns, lookup_ns);
    if (hit) {
        aot_perf_add_u64(&aot_perf_counters.lookup_hit_count, 1);
    } else {
        aot_perf_add_u64(&aot_perf_counters.lookup_reject_count, 1);
    }
    if (page_compare_equal) {
        aot_perf_add_u64(&aot_perf_counters.page_compare_match_count, 1);
    } else if (page_hash_equal) {
        aot_perf_add_u64(&aot_perf_counters.page_compare_reject_count, 1);
    }
}

void aot_perf_record_register_activate_tb(uint32_t aot_mode, uint32_t tb_count,
        uint32_t registered_activated_count, uint64_t register_activate_ns)
{
    (void)aot_mode;
    (void)tb_count;

    if (!aot_perf_enabled()) {
        return;
    }

    aot_perf_add_u64(&aot_perf_counters.register_activate_page_count, 1);
    aot_perf_add_u64(&aot_perf_counters.register_activate_tb_count,
            registered_activated_count);
    aot_perf_add_u64(&aot_perf_counters.register_activate_ns,
            register_activate_ns);
}

void aot_perf_emit_summary(void)
{
    uint64_t mmap_core, mmap_user, index_core, index_user;
    uint64_t lookup_ns, register_activate_ns, aot_load_ns;
    uint64_t loaded_tb_core, loaded_tb_user;

    if (!aot_perf_enabled()) {
        return;
    }
    if (__sync_lock_test_and_set(&aot_perf_summary_emitted, 1)) {
        return;
    }

    mmap_core = aot_perf_read_u64(&aot_perf_counters.mmap_ns_core);
    mmap_user = aot_perf_read_u64(&aot_perf_counters.mmap_ns_user);
    index_core = aot_perf_read_u64(&aot_perf_counters.index_build_ns_core);
    index_user = aot_perf_read_u64(&aot_perf_counters.index_build_ns_user);
    lookup_ns = aot_perf_read_u64(&aot_perf_counters.runtime_lookup_ns);
    register_activate_ns =
        aot_perf_read_u64(&aot_perf_counters.register_activate_ns);
    loaded_tb_core = aot_perf_read_u64(&aot_perf_counters.loaded_tb_core);
    loaded_tb_user = aot_perf_read_u64(&aot_perf_counters.loaded_tb_user);
    aot_load_ns = mmap_core + mmap_user + index_core + index_user +
        lookup_ns + register_activate_ns;

    aot_perf_emit_event_begin("runtime_lookup_summary");
    fprintf(stderr,
            ",\"lookup_count\":%llu,\"lookup_hit_count\":%llu,"
            "\"lookup_reject_count\":%llu,\"lookup_probe_total\":%llu,"
            "\"lookup_probe_max\":%llu,"
            "\"page_compare_match_count\":%llu,"
            "\"page_compare_reject_count\":%llu,"
            "\"runtime_lookup_seconds\":%.9f}\n",
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_hit_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_reject_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_probe_total),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_probe_max),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.page_compare_match_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.page_compare_reject_count),
            aot_perf_seconds(lookup_ns));
    aot_perf_emit_event_begin("register_activate_tb_summary");
    fprintf(stderr,
            ",\"register_activate_page_count\":%llu,"
            "\"register_activate_tb_count\":%llu,"
            "\"register_activate_seconds\":%.9f}\n",
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.register_activate_page_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.register_activate_tb_count),
            aot_perf_seconds(register_activate_ns));
    aot_perf_emit_event_begin("summary");
    fprintf(stderr,
            ",\"loaded_pages_total\":%llu,"
            "\"loaded_tb_core\":%llu,\"loaded_tb_user\":%llu,"
            "\"loaded_tb_total\":%llu,"
            "\"aot_mmap_seconds_core\":%.9f,"
            "\"aot_mmap_seconds_user\":%.9f,"
            "\"aot_index_build_seconds_core\":%.9f,"
            "\"aot_index_build_seconds_user\":%.9f,"
            "\"aot_runtime_lookup_seconds\":%.9f,"
            "\"aot_tb_register_activate_seconds\":%.9f,"
            "\"aot_load_seconds\":%.9f}\n",
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.loaded_pages_total),
            (unsigned long long)loaded_tb_core,
            (unsigned long long)loaded_tb_user,
            (unsigned long long)(loaded_tb_core + loaded_tb_user),
            aot_perf_seconds(mmap_core),
            aot_perf_seconds(mmap_user),
            aot_perf_seconds(index_core),
            aot_perf_seconds(index_user),
            aot_perf_seconds(lookup_ns),
            aot_perf_seconds(register_activate_ns),
            aot_perf_seconds(aot_load_ns));
}

static bool aot_load_file_validate_index(void *mem_ptr, uint64_t file_size,
        AOTLoadedFileIndex *index)
{
    aot_tail *tail;
    size_t page_info_size;

    if (file_size < sizeof(aot_tail)) {
        fprintf(stderr, "[AOT] Load: invalid file size\n");
        return false;
    }

    tail = (aot_tail *)((uint8_t *)mem_ptr + file_size - sizeof(aot_tail));
    if (tail->page_num < 0) {
        fprintf(stderr, "[AOT] Load: invalid page count: %d\n",
                tail->page_num);
        return false;
    }

    index->page_num = (uint32_t)tail->page_num;
    if (!aot_page_info_size(index->page_num, &page_info_size) ||
            page_info_size > file_size - sizeof(aot_tail)) {
        fprintf(stderr, "[AOT] Load: invalid page info size\n");
        return false;
    }

    index->tail = tail;
    index->info_array =
        (page_info_array_t *)((uint8_t *)tail - page_info_size);
    if (index->info_array->page_count != index->page_num) {
        fprintf(stderr, "[AOT] Load: page count mismatch\n");
        return false;
    }

    return true;
}

static bool aot_load_file_prepare_hash(AOTLoadedFileIndex *index)
{
    uint64_t hash_target = (uint64_t)index->page_num * 2;

    if (hash_target > (UINT32_C(1) << 31)) {
        fprintf(stderr, "[AOT] Load: invalid hash size\n");
        return false;
    }

    index->hash_size = 1;
    while ((uint64_t)index->hash_size < hash_target) {
        index->hash_size <<= 1;
    }

    size_t count = index->hash_size;
    if (count > (SIZE_MAX - 63) / sizeof(*index->hash_table)) {
        return false;
    }
    size_t bytes = ROUND_UP(count * sizeof(*index->hash_table), 64);
    index->hash_table = aligned_alloc(64, bytes);
    if (!index->hash_table) {
        fprintf(stderr, "[AOT] Load: alloc hash table failed\n");
        return false;
    }
    memset(index->hash_table, -1, bytes);
    return true;
}

static void aot_load_file_build_hash(AOTLoadedFileIndex *index)
{
    for (uint32_t i = 0; i < index->info_array->page_count; i++) {
        uint32_t h = index->info_array->pages[i].page_hash;
        uint32_t idx = h & (index->hash_size - 1);
        uint32_t probe_len = 0;

        while (index->hash_table[idx].offset != UINT64_MAX) {
            index->bucket_collision_count++;
            probe_len++;
            idx = (idx + 1) & (index->hash_size - 1);
        }
        if (probe_len > index->linear_probe_max) {
            index->linear_probe_max = probe_len;
        }
        index->hash_table[idx] = (AOTPageIndexEntry) {
            .offset = index->info_array->pages[i].file_offset,
            .hash = h,
        };
    }
}

AOTPublishedFile *load_aot_file(const char *file_name, uint32_t aot_mode)
{
    AOTPublishedFile *published = NULL;
    void *mem_ptr = NULL;
    uint64_t size = 0;
    bool perf_enabled = aot_perf_enabled();
    uint64_t mmap_begin = perf_enabled ? aot_perf_now_ns() : 0;
    uint64_t index_begin;
    AOTLoadedFileIndex index = { 0 };

    size_t aot_file_name_len = strlen(aot_file_path) + strlen(file_name) + 1;
    char *aot_file_name = malloc(aot_file_name_len);
    if (!aot_file_name) {
        fprintf(stderr, "[AOT] Load: alloc file name failed\n");
        return NULL;
    }
    snprintf(aot_file_name, aot_file_name_len, "%s%s", aot_file_path, file_name);
    if (access(aot_file_name, 0) < 0) {
        goto fail_no_lock;
    }

    int lockfd = -1;
    char *aot_file_lock = gen_file_lock(aot_file_name);
    if (!aot_file_lock) {
        goto fail_no_lock;
    }
    if (file_lock(aot_file_lock, &lockfd, F_RDLCK, false) < 0) {
        if (lockfd >= 0) {
            close(lockfd);
        }
        goto fail_lock;
    }

    mem_ptr = load_file_to_mem(aot_file_name, &size, aot_mode);
    if (!mem_ptr) {
        aot_perf_record_load_file(aot_mode, aot_file_name, size, 0, 0,
                false, perf_enabled ? aot_perf_now_ns() - mmap_begin : 0);
        goto fail;
    }

    if (!aot_load_file_validate_index(mem_ptr, size, &index) ||
            !aot_load_file_prepare_hash(&index)) {
        goto fail;
    }

    aot_perf_record_load_file(aot_mode, aot_file_name, size,
            index.tail->page_num, index.tail->tb_num, true,
            perf_enabled ? aot_perf_now_ns() - mmap_begin : 0);
    index_begin = perf_enabled ? aot_perf_now_ns() : 0;
    aot_load_file_build_hash(&index);
    aot_perf_record_build_index(aot_mode, index.tail->page_num,
            index.hash_size, index.bucket_collision_count,
            index.linear_probe_max,
            perf_enabled ? aot_perf_now_ns() - index_begin : 0);
    published = g_try_new0(AOTPublishedFile, 1);
    if (!published) {
        goto fail;
    }
    published->tb_ptr = mem_ptr;
    published->size = size;
    published->hash_table = index.hash_table;
    published->hash_size = index.hash_size;
    published->generation = aot_alloc_file_generation();
    index.hash_table = NULL;

    aot_verbose_log("[AOT][Load] Opened: %s, pages=%u, hash_size=%u\n",
            aot_file_name, index.page_num, index.hash_size);

    flock_set(lockfd, F_UNLCK, true);
    close(lockfd);
    free(aot_file_lock);
    free(aot_file_name);
    return published;

fail:
    flock_set(lockfd, F_UNLCK, true);
    close(lockfd);
    free(aot_file_lock);
    free(index.hash_table);
    free(published);
    if (mem_ptr) {
        munmap(mem_ptr, size);
    }
    remove(aot_file_name);
    free(aot_file_name);
    return NULL;

fail_lock:
    free(aot_file_lock);
fail_no_lock:
    free(aot_file_name);
    return NULL;
}

/* __thread bool aot_load_ok; */
static inline int recover_tb(TranslationBlock *curr_tb, aot_block *aot_tb, int *search_size,
        aot_rel *rel_table, uint32_t rel_count, void *code_highwater, ENV *lsenv, int mtcc_id)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    int ir2_size = aot_tb->ir2_num << 2;
    const void* code_end = curr_tb->tc.ptr + ir2_size;
    if (!option_split_search) {
        code_end += aot_tb->search_size;
    }
    if (code_end >= code_highwater) {
        return -1;
    } else {
        curr_tb->preorder_tb_offset = 0;
        curr_tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
        curr_tb->s_data->tu_cold_exit_offset[0] = TU_COLD_EXIT_INVALID;
        curr_tb->s_data->tu_cold_exit_offset[1] = TU_COLD_EXIT_INVALID;
        curr_tb->jit_tucheck_external[0] = NULL;
        curr_tb->jit_tucheck_external[1] = NULL;
        curr_tb->ras_dest = (uintptr_t)NULL;
#endif
        curr_tb->page_next[0] = 0;
        curr_tb->page_next[1] = 0;
        curr_tb->jmp_inst_hold[0] = 0;
        curr_tb->jmp_inst_hold[1] = 0;

        curr_tb->flags = aot_tb->flags;
        curr_tb->cflags = aot_tb->cflags;
        curr_tb->icount = aot_tb->icount;
        curr_tb->size = aot_tb->size;
        curr_tb->ir2_num = aot_tb->ir2_num;
        curr_tb->jmp_reset_offset[0] = aot_tb->jmp_reset_offset[0];
        curr_tb->jmp_reset_offset[1] = aot_tb->jmp_reset_offset[1];
        curr_tb->jmp_insn_offset[0]  = aot_tb->jmp_insn_offset[0];
        curr_tb->jmp_insn_offset[1]  = aot_tb->jmp_insn_offset[1];
        curr_tb->jmp_target_addr[0] = aot_tb->jmp_target_addr[0];
        curr_tb->jmp_target_addr[1] = aot_tb->jmp_target_addr[1];
        curr_tb->next_pc[0] = aot_tb->next_pc[0] + ((int64_t)curr_tb->pc - (int64_t)aot_tb->pc);
        curr_tb->next_pc[1] = aot_tb->next_pc[1] + ((int64_t)curr_tb->pc - (int64_t)aot_tb->pc);
        curr_tb->link_opt[0] = aot_tb->link_opt[0];
        curr_tb->link_opt[1] = aot_tb->link_opt[1];
        curr_tb->int_opt[0] = aot_tb->int_opt[0];
        curr_tb->int_opt[1] = aot_tb->int_opt[1];
        /* curr_tb->preorder_tb_offset = aot_tb->preorder_tb_offset; */
        curr_tb->inst_ptr_offset = aot_tb->inst_ptr_offset;
        LATXS_RAS_TP_COLD_OFFSET(curr_tb) = 0;
        LATXS_RAS_TP_COLD_INSNS(curr_tb) = 0;
        if (option_rastoptp && curr_tb->inst_ptr_offset) {
            LATXS_RAS_TP_COLD_OFFSET(curr_tb) = aot_tb->ras_tp_cold_offset;
            LATXS_RAS_TP_COLD_INSNS(curr_tb) = LATX_RAS_TP_CALL_COLD_INSNS;
        }
        curr_tb->label_pos_nocheck = aot_tb->label_pos_nocheck;
        curr_tb->misc_flags = aot_tb->misc_flags;
        /* curr_tb->page_next[0] = aot_tb->page_next[0]; */
        /* curr_tb->page_next[1] = aot_tb->page_next[1]; */
        curr_tb->jmp_cp_data[0] = aot_tb->jmp_cp_data[0];
        curr_tb->jmp_cp_data[1] = aot_tb->jmp_cp_data[1];
        /* curr_tb->jmp_inst_hold[0] = aot_tb->jmp_inst_hold[0]; */
        /* curr_tb->jmp_inst_hold[1] = aot_tb->jmp_inst_hold[1]; */

        void* aot_ir2_code = (void *)((uintptr_t)aot_tb + sizeof(aot_block));
        if (option_split_search) {
            memcpy((void*)curr_tb->tc.ptr, aot_ir2_code, ir2_size);
            if (!region_alloc_search(curr_tb, mtcc_id)) {
                return -1;
            }
            memcpy((void*)curr_tb->search_ptr, aot_ir2_code + ir2_size, aot_tb->icount << 2);
            *search_size = 0;
        } else {
            memcpy((void*)curr_tb->tc.ptr, aot_ir2_code, ir2_size + aot_tb->search_size);
            *search_size = aot_tb->search_size;
            curr_tb->search_size = aot_tb->search_size;
        }
        aot_do_tb_reloc(lsenv, curr_tb, rel_table, rel_count);

#if DEBUG_AOT
        if (option_aot_debug) {
            fprintf(stderr, "aot_ir2_code point %p\n", aot_ir2_code);
            fprintf(stderr, "tb pc %lx %lx\n", curr_tb->pc, aot_tb->pc);
            fprintf(stderr, "ir2_num %d %d\n", curr_tb->ir2_num, aot_tb->ir2_num);
            uint32_t *buf0 = (uint32_t *)curr_tb->tc.ptr;
            /* uint32_t *buf0 = (uint32_t *)aot_ir2_code; */
            for (int i = 0; i < aot_tb->ir2_num; i++) {
                fprintf(stderr, "i %d %x\n", i, buf0[i]);
            }
            /* aot_load_ok = true; */
        }

        recover_time += get_tsc() - begin;
#endif

        return ir2_size;
    }
    return 0;
}

static uint64_t find_page_with_hash(ADDR hva, uint32_t h,
        uint64_t perf_begin)
{
    bool perf_enabled = aot_perf_enabled();
    uint32_t probe_len = 0;
    bool page_hash_equal = false;
    bool page_compare_equal = false;
    int idx = (h) & (aot_hash_size - 1);
    int i = 0;
#if DEBUG_AOT
    uint64_t for_begin = get_tsc();
#endif
    for (i = 0; i < aot_hash_size; i++)
    {
        uint64_t file_disp = hash_table[idx].offset;
        probe_len++;
        if (file_disp == UINT64_MAX) {
            break;
        }
        if (!option_arch_opt || hash_table[idx].hash == h) {
#if DEBUG_AOT
            uint64_t read_begin = get_tsc();
#endif
            uint64_t file_addr = (uint64_t)in_tb_ptr + file_disp;
            page_head *ph = (page_head *)file_addr;
            void *guest_code = (void *)(file_addr+ sizeof(page_head));
#if DEBUG_AOT
            read_time += get_tsc() - read_begin;
            uint64_t cmp_begin = get_tsc();
#endif
            if (ph->page_magic == AOT_PAGE_MAGIC &&
                    (option_arch_opt || ph->page_hash == h)) {
                page_hash_equal = true;
                if (memcmp((void*)hva, guest_code, TARGET_PAGE_SIZE) == 0) {
                    page_compare_equal = true;
#if DEBUG_AOT
                    cmp_time += get_tsc() - cmp_begin;
                    load_page_count++;
                    for_time += get_tsc() - for_begin;
#endif
                    if (perf_enabled) {
                        aot_perf_record_lookup(true, probe_len, page_hash_equal,
                                page_compare_equal,
                                aot_perf_now_ns() - perf_begin);
                    }
                    return file_addr;
                }
            } else if (ph->page_magic != AOT_PAGE_MAGIC) {
                fprintf(stderr, "find page bad page_magic %x\n", ph->page_magic);
                exit(0);
            }
#if DEBUG_AOT
            cmp_time += get_tsc() - cmp_begin;
#endif
        }
        idx = (idx + 1) & (aot_hash_size - 1);
    }
#if DEBUG_AOT
    for_time += get_tsc() - for_begin;
    mis_page_count++;
#endif

    if (perf_enabled) {
        aot_perf_record_lookup(false, probe_len, page_hash_equal,
                page_compare_equal, aot_perf_now_ns() - perf_begin);
    }
    return 0;
}

uint64_t find_page(ADDR hva)
{
    bool perf_enabled = aot_perf_enabled();
    uint64_t perf_begin = perf_enabled ? aot_perf_now_ns() : 0;
#if DEBUG_AOT
    uint64_t hash_begin = get_tsc();
#endif
    uint32_t h = get_page_hash((uint32_t *)(hva & TARGET_PAGE_MASK));
#if DEBUG_AOT
    hash_time += get_tsc() - hash_begin;
#endif

    return find_page_with_hash(hva, h, perf_begin);
}

#ifdef CONFIG_LATX_TU
static uint64_t find_aot_tu_page(ADDR hva, tb_page_addr_t phys_page)
{
    AOTTUPageCache cache;
    bool cache_valid = get_page_aot_tu_cache(phys_page, &cache);

    if (cache_valid && cache.file_generation == aot_file_generation) {
        if (cache.file_offset == UINT64_MAX) {
            return 0;
        }
        return (uint64_t)in_tb_ptr + cache.file_offset;
    }

    uint64_t file_addr = find_page(hva);
    uint64_t file_offset = file_addr ?
        file_addr - (uint64_t)in_tb_ptr : UINT64_MAX;
    set_page_aot_tu_cache(phys_page, aot_file_generation, file_offset);
    return file_addr;
}
#endif

#ifdef CONFIG_LATX_TU
/* A candidate page is not necessarily the guest's current fetch address. */
static tb_page_addr_t aot_probe_code_page(CPUArchState *env, vaddr pc,
                                         void **hostp)
{
    int saved_error_code = env->error_code;
    tb_page_addr_t pp = get_page_addr_code_hostp_noexit(env, pc, hostp);

    /* A failed x86 probe writes error_code even without raising a fault. */
    env->error_code = saved_error_code;
    return pp;
}

static uint64_t aot_find_tu_page_record(CPUState *cpu, vaddr page_pc,
        ADDR *page_hva, tb_page_addr_t *page_phys)
{
    CPUArchState *env = cpu->env_ptr;

    for (int retry = 0; retry < 8; retry++) {
        void *host_page = NULL;
        tb_page_addr_t phys = aot_probe_code_page(env, page_pc,
                &host_page);

        if (phys == (tb_page_addr_t)-1 ||
                !aot_host_pc_in_main_ram(host_page)) {
            return 0;
        }
        phys &= TARGET_PAGE_MASK;
        tb_lock_page0(phys);

        void *locked_host_page = NULL;
        tb_page_addr_t locked_phys = aot_probe_code_page(env,
                page_pc, &locked_host_page);
        if (locked_phys == (tb_page_addr_t)-1 ||
                (locked_phys & TARGET_PAGE_MASK) != phys ||
                !aot_host_pc_in_main_ram(locked_host_page)) {
            tb_unlock_page0(phys);
            continue;
        }

#ifdef CONFIG_SOFTMMU
#ifdef CONFIG_LATX_HACK_MEM
        if (!hamt_ptwflags_smc_page_is_protected(phys)) {
            tlb_protect_code(phys);
        }
#else
        tlb_protect_code(phys);
#endif
#endif
        uint64_t file_addr = find_aot_tu_page((ADDR)locked_host_page,
                phys >> TARGET_PAGE_BITS);
        tb_unlock_page0(phys);
        if (page_hva) {
            *page_hva = (ADDR)locked_host_page;
        }
        if (page_phys) {
            *page_phys = phys;
        }
        return file_addr;
    }
    return 0;
}
#endif

static TranslationBlock *activate_tb(TranslationBlock *tb, CPUState *cpu,
        ENV *lsenv)
{

    if (!tb) {
        return NULL;
    }
    if (!(tb->cflags & CF_NO_ACTIVE)) {
        fprintf(stderr, "reactivate tb %lx tb->cflags %x\n", tb->pc, tb->cflags);
        /* exit(0); */
        return NULL;
    }

    int search_size;
    CPUArchState *env = cpu->env_ptr;
    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    tcg_insn_unit *gen_code_buf = tcg_ctx->code_gen_ptr;

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    /* set TBMini */
    if (qemu_tcg_mode_2_lat()) {
        uint64_t *tbm = (uint64_t *)gen_code_buf;
        tbmini_set_pointer(tbm, (uint64_t)tb);
        gen_code_buf = (tcg_insn_unit *)((uintptr_t)gen_code_buf + sizeof(struct TBMini));
    }
#endif

    uintptr_t file_disp = (uintptr_t)tb->tc.ptr;
    if (file_disp < 0x1000 || *(uint32_t *)file_disp != AOT_TB_MAGIC) {
        uint64_t phys_pc = tb_page_addr0(tb);
        ADDR hva = (ADDR)qemu_ram_addr_to_host(phys_pc & TARGET_PAGE_MASK);
        uint64_t page_addr = find_page(hva);
        file_disp = 0;
        if (page_addr) {
            page_head *ph = (page_head *)page_addr;
            uintptr_t curr = page_addr + sizeof(page_head) + TARGET_PAGE_SIZE;
            uint32_t n = 0;
            while (n < ph->tb_count) {
                aot_block *ab = (aot_block *)(curr + 16);
                if (ab->pc == tb->pc && ab->flags == tb->flags
                        && (ab->cflags & ~CF_INVALID) == (tb->cflags & ~(CF_NO_ACTIVE | CF_INVALID))) {
                    file_disp = curr;
                    break;
                }
                curr += *(uint32_t *)(curr + 4);
                n++;
            }
        }
        if (!file_disp) return NULL;
    }
    tb->tc.ptr = (void *)tcg_splitwx_to_rx(gen_code_buf);

    uint32_t magic = *(uint32_t *)file_disp;
    if (magic != AOT_TB_MAGIC) {
        fprintf(stderr, "error tb magic %x in activate_tb tid %ld\n", magic, syscall(SYS_gettid));
        _exit(0);
    }

    tb->misc_flags &= ~MF_FLAGS_NOEFLAGS;
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    set_mask_to_miscflags(tb, curr_mtcc(env),
            MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT);

    uint32_t all_tb_size = *(uint32_t *)(file_disp + 8);
    uint32_t rel_count = *(uint32_t *)(file_disp + 12);
    aot_block *file_tb = (aot_block *)(file_disp + 16);
    aot_rel *curr_tb_rel_table =
        (aot_rel *)((void *)file_tb + all_tb_size);

    int gen_code_size = tb->ir2_num << 2;

    void* aot_ir2_code = (void *)((uintptr_t)file_tb + sizeof(aot_block));

    int copy_size = gen_code_size;
    if (!option_split_search) {
        search_size = tb->search_size;
        copy_size += search_size;
    } else {
        search_size = 0;
        if (!region_alloc_search(tb, mtcc_id)) {
            fprintf(stderr, "region_alloc_search fail \n");
            exit(0);
            return NULL;
        }
        memcpy((void *)tb->search_ptr,
                aot_ir2_code + (tb->ir2_num << 2), tb->icount << 2);
    }

    if (option_pushpop) {
        if (!region_alloc_push_pop_data(tb, mtcc_id, tb->icount)) {
            return NULL;
        }
        memcpy(tb_opt_ptr(tb), aot_ir2_code + (tb->ir2_num << 2) + (tb->icount << 2),
                tb->icount << 2);
    }

    if (tb->tc.ptr + copy_size >= tcg_ctx->code_gen_highwater) {
        return NULL;
    }

    memcpy((void*)tb->tc.ptr, aot_ir2_code, copy_size);
    aot_do_tb_reloc(lsenv, tb, curr_tb_rel_table, rel_count);

    tb->preorder_tb_offset = 0;
    tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
    tb->ras_dest = (uintptr_t)NULL;
#endif
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    /* set_mask_to_miscflags(tb, curr_mtcc(env), */
    /*         MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT); */
    /* remove_misc_flags(tb, MF_LINK0_MASK | MF_LINK1_MASK | MF_LINK_STATUS); */
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 0);
        } else {
            tb_reset_jump(tb, 0);
        }
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 1);
        } else {
            tb_reset_jump(tb, 1);
        }
    }
    if (tb) {
        /* 3. flush icache range */
        ADDR s = (ADDR)tb->tc.ptr;
        ADDR e = s + gen_code_size;
        flush_idcache_range((uintptr_t)s, (uintptr_t)s, (uintptr_t)e);
    }


#if defined(CONFIG_LATX_TCG_MODE) || defined(CONFIG_TCG_REGION_TREE_ENABLE)
    tb->tc.size = gen_code_size;
#endif

    /*
     * For CF_PCREL, attribute all executions of the generated code
     * to its first mapping.
     */
    /* perf_report_code(tb->pc, tb, tcg_splitwx_to_rx(gen_code_buf)); */

    uintptr_t sptr = (uintptr_t)gen_code_buf + gen_code_size;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        sptr = sptr + sizeof(struct TBMini);
    }
#endif
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
        ROUND_UP(sptr + search_size, CODE_GEN_ALIGN));

    /* The source is invalid until publication. Keep r2 gates relinkable;
     * the CALL cold path can request a safe link after publication. */
    if (option_soft_ras && !option_rastoptp && tb->inst_ptr_offset &&
            ((tb->pc & TARGET_PAGE_MASK) ==
             (tb->s_data->x86_next_pc & TARGET_PAGE_MASK))) {
        TranslationBlock *next_tb = aot_tb_lookup(cpu,
                tb->s_data->x86_next_pc, tb->cs_base,
                tb->flags, tb->cflags | CF_INVALID | CF_NO_ACTIVE);
        if (next_tb && (tb_cflags(next_tb) & CF_NO_ACTIVE)) {
            next_tb = activate_tb(next_tb, cpu, lsenv);
        } else if (!next_tb) {
            next_tb = tb_lookup(cpu, tb->s_data->x86_next_pc, tb->cs_base,
                    tb->flags, tb->cflags & ~(CF_INVALID | CF_NO_ACTIVE));
        }
        if (next_tb) {
            if (!latxs_ras_link(tb, next_tb)) {
                latxs_ras_unlink(tb);
            }
        }
    }

    tb = aot_tb_relink_page(tb);
    if (!tb) {
        return NULL;
    }

#if DEBUG_AOT
    load_core_tb_count++;
#endif
    return tb;
}

inline int recover_tb_head(TranslationBlock *curr_tb, aot_block *aot_tb)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif

    curr_tb->bool_flags &= ~IS_HRAS_CALL;
    curr_tb->bool_flags |= IS_AOT_TB;
    curr_tb->flags = aot_tb->flags;
    curr_tb->cflags = aot_tb->cflags;
    if (!option_split_search) {
        curr_tb->search_size = aot_tb->search_size;
    }
    curr_tb->preorder_tb_offset = 0;
    curr_tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
    curr_tb->s_data->tu_cold_exit_offset[0] = TU_COLD_EXIT_INVALID;
    curr_tb->s_data->tu_cold_exit_offset[1] = TU_COLD_EXIT_INVALID;
    curr_tb->jit_tucheck_external[0] = NULL;
    curr_tb->jit_tucheck_external[1] = NULL;
    curr_tb->ras_dest = (uintptr_t)NULL;
#endif
    curr_tb->page_next[0] = 0;
    curr_tb->page_next[1] = 0;
    curr_tb->jmp_inst_hold[0] = 0;
    curr_tb->jmp_inst_hold[1] = 0;
    curr_tb->signal_unlink[0] = 0;
    curr_tb->signal_unlink[1] = 0;

    curr_tb->icount = aot_tb->icount;
    curr_tb->size = aot_tb->size;
    curr_tb->ir2_num = aot_tb->ir2_num;
    curr_tb->jmp_reset_offset[0] = aot_tb->jmp_reset_offset[0];
    curr_tb->jmp_reset_offset[1] = aot_tb->jmp_reset_offset[1];
    curr_tb->jmp_insn_offset[0]  = aot_tb->jmp_insn_offset[0];
    curr_tb->jmp_insn_offset[1]  = aot_tb->jmp_insn_offset[1];
    curr_tb->jmp_target_addr[0] = aot_tb->jmp_target_addr[0];
    curr_tb->jmp_target_addr[1] = aot_tb->jmp_target_addr[1];
    curr_tb->link_opt[0] = aot_tb->link_opt[0];
    curr_tb->link_opt[1] = aot_tb->link_opt[1];
    curr_tb->int_opt[0] = aot_tb->int_opt[0];
    curr_tb->int_opt[1] = aot_tb->int_opt[1];
    /* curr_tb->preorder_tb_offset = aot_tb->preorder_tb_offset; */
    curr_tb->inst_ptr_offset = aot_tb->inst_ptr_offset;
    LATXS_RAS_TP_COLD_OFFSET(curr_tb) = 0;
    LATXS_RAS_TP_COLD_INSNS(curr_tb) = 0;
    if (option_rastoptp && curr_tb->inst_ptr_offset) {
        LATXS_RAS_TP_COLD_OFFSET(curr_tb) = aot_tb->ras_tp_cold_offset;
        LATXS_RAS_TP_COLD_INSNS(curr_tb) = LATX_RAS_TP_CALL_COLD_INSNS;
    }
    curr_tb->label_pos_nocheck = aot_tb->label_pos_nocheck;
    curr_tb->misc_flags = aot_tb->misc_flags;
    /* curr_tb->page_next[0] = aot_tb->page_next[0]; */
    /* curr_tb->page_next[1] = aot_tb->page_next[1]; */
    curr_tb->jmp_cp_data[0] = aot_tb->jmp_cp_data[0];
    curr_tb->jmp_cp_data[1] = aot_tb->jmp_cp_data[1];
    curr_tb->s_data->tu_generation_offset_patch[0] =
        aot_tb->tu_generation_offset_patch[0];
    curr_tb->s_data->tu_generation_offset_patch[1] =
        aot_tb->tu_generation_offset_patch[1];
    /* curr_tb->jmp_inst_hold[0] = aot_tb->jmp_inst_hold[0]; */
    /* curr_tb->jmp_inst_hold[1] = aot_tb->jmp_inst_hold[1]; */


    int64_t pc_diff = (int64_t)curr_tb->pc - (int64_t)aot_tb->pc;
    if (aot_tb->next_pc[0]) {
        curr_tb->next_pc[0] = aot_tb->next_pc[0] + pc_diff;
    } else {
        curr_tb->next_pc[0] = 0;
    }
    if (aot_tb->next_pc[1]) {
        curr_tb->next_pc[1] = aot_tb->next_pc[1] + pc_diff;
    } else {
        curr_tb->next_pc[1] = 0;
    }
    curr_tb->s_data->x86_next_pc = aot_tb->x86_next_pc + pc_diff;
#ifdef CONFIG_LATX_TU
    curr_tb->tc.offset_in_tu = aot_tb->tc.offset_in_tu;
    curr_tb->s_data->x86_target_pc = aot_tb->x86_target_pc + pc_diff;
    curr_tb->s_data->tu_size = aot_tb->tu_size;
    /* curr_tb->s_data->is_first_tb = 0; */
    /* curr_tb->s_data->last_ir1_type = 0; */
    /* curr_tb->s_data->tu_tb_mode = TU_TB_MODE_NONE; */
    curr_tb->s_data->tu_jmp[TU_TB_INDEX_TARGET] =
        aot_tb->tu_jmp[TU_TB_INDEX_TARGET];
    curr_tb->s_data->tu_jmp[TU_TB_INDEX_NEXT] =
        aot_tb->tu_jmp[TU_TB_INDEX_NEXT];
    curr_tb->s_data->eflag_use = aot_tb->eflag_use;
    curr_tb->s_data->eflag_use_real = aot_tb->eflag_use_real;
    curr_tb->s_data->last_ir1_type = aot_tb->last_ir1_type;
    curr_tb->s_data->tu_tb_mode = aot_tb->tu_tb_mode;
    /* curr_tb->tu_unlink_stub_offset = TU_UNLINK_STUB_INVALID; */
    curr_tb->ptn.eflag_type = aot_tb->ptn.eflag_type;
    if (curr_tb->ptn.eflag_type != PTN_EFLAG_TYPE_INVALID) {
        curr_tb->ptn.os = aot_tb->ptn.os;
        curr_tb->ptn.opnd0 = aot_tb->ptn.opnd0;
        curr_tb->ptn.opnd1 = aot_tb->ptn.opnd1;
        tb_set_ptn_pcoffset(curr_tb, aot_tb->ptn.pcoffset);
    }
#endif

#if DEBUG_AOT
    if (option_aot_debug) {
        fprintf(stderr, "tb pc %lx %lx\n", curr_tb->pc, aot_tb->pc);
        fprintf(stderr, "ir2_num %d %d\n", curr_tb->ir2_num, aot_tb->ir2_num);
        fprintf(stderr, "inst_ptr_offset %x %x\n",
                curr_tb->inst_ptr_offset, aot_tb->inst_ptr_offset);
        fprintf(stderr, "label_pos_nocheck %x %x\n",
                curr_tb->label_pos_nocheck, aot_tb->label_pos_nocheck);
        fprintf(stderr, "misc_flags %x %x\n", curr_tb->misc_flags, aot_tb->misc_flags);
    }

    recover_time += get_tsc() - begin;
#endif
    return curr_tb->ir2_num << 2;
}

#ifdef CONFIG_LATX_PRINT
int tb_aot_cnt[8];
#endif

static __attribute__((unused)) TranslationBlock *register_tb(vaddr exec_addr,
        CPUState *cpu, vaddr pc,
        uint64_t cs_base, aot_block *file_tb,
        aot_rel *rel_table, int rel_count, ENV *lsenv, uint64_t file_disp)
{
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb, *existing_tb;

    tb_page_addr_t phys_pc;
    tcg_insn_unit *gen_code_buf;
    void *host_pc;

    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];

    assert_memory_lock();
    qemu_thread_jit_write();

    phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

    tb = tcg_tb_alloc(tcg_ctx);
    if (unlikely(!tb)) {
        /* fprintf(stderr, "tcg_tb_alloc fail\n"); */
        return NULL;
    }

    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->pc = pc;
    tb->cs_base = cs_base;
#ifdef CONFIG_LATX_TU
    tu_reset_tb(tb);
#endif
    recover_tb_head(tb, file_tb);
    tb->tc.ptr = (void *)file_disp;
    tb_set_page_addr0(tb, phys_pc);
    tb_next_data_reset(tb);

    if (option_fast_itlb) {
        tb->hva = phys_pc & TARGET_PAGE_MASK;
    } else {
#ifdef CONFIG_SOFTMMU
        int error = 0;
        tb->hva = latxs_tr_cvt_cpu_addr_code(env, pc, &error);
#else
        tb->hva = 0;
#endif
    }
    tb_set_page_addr1(tb, -1);

#ifdef CONFIG_LATX_PROFILER
    CLN_TB_PROFILE(tb);
#endif
    /* init jump list */
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    /*
     * Insert TB into the corresponding region tree before publishing it
     * through QHT. Otherwise rewinding happened in the TB might fail to
     * lookup itself using host PC.
     */
    tcg_tb_insert(tb);

#ifdef CONFIG_LATX_PRINT
    if (tb->flags & HF_CPL_MASK) {
        tb_aot_cnt[cpu->cpu_index + 4]++;
    } else {
        tb_aot_cnt[cpu->cpu_index]++;
    }
#endif

    /*
     * No explicit memory barrier is required -- tb_link_page() makes the
     * TB visible in a consistent state.
     */
    tb->cflags |= CF_NO_ACTIVE | CF_INVALID;
    existing_tb = aot_tb_link_page(tb, false);

    /* fprintf(stderr, "register tb %lx tid %ld\n", tb->pc, syscall(SYS_gettid)); */

//    assert_no_pages_locked();
    /* if the TB already exists, discard what we just translated */
    if (unlikely(existing_tb != tb)) {
        fprintf(stderr, "existing_tb\n");
        uintptr_t orig_aligned = (uintptr_t)gen_code_buf;

        if (!option_split_tb) {
            orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
        }
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
        tcg_tb_remove(tb);
        return existing_tb;
    }
#if DEBUG_AOT
    register_tb_count++;
#endif

    return tb;
}

static G_NORETURN void aot_request_tb_flush(CPUState *cpu,
        uint32_t mtcc_id, tb_page_addr_t phys_pc)
{
#ifdef CONFIG_LATX_TU
    aot_clear_tu_conflicts();
#endif
    aot_unlock_load_pages(phys_pc);
    tb_flush_mtcc(cpu, mtcc_id);
#ifdef CONFIG_LATX_TU
    if (tu_runtime_enabled()) {
        tu_trees_reset();
        tu_reset_tb_flush();
    }
#endif
    cpu->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cpu);
}

static TranslationBlock *register_and_activate_tb(vaddr exec_addr,
        CPUState *cpu, vaddr pc,
        uint64_t cs_base, aot_block *file_tb,
        aot_rel *rel_table, int rel_count, ENV *lsenv, uint64_t file_disp)
{
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb, *existing_tb;
    tb_page_addr_t phys_pc;
    tcg_insn_unit *gen_code_buf;
    void *host_pc;
    int search_size;
    int gen_code_size = file_tb->ir2_num << 2;
    int copy_size = gen_code_size;
    size_t code_prefix = 0;

    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];

    assert_memory_lock();
    qemu_thread_jit_write();

    phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

    if (!option_split_search) {
        copy_size += file_tb->search_size;
    }
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        code_prefix = sizeof(struct TBMini);
    }
#endif
    code_prefix += CODE_GEN_ALIGN - 1;

    if ((uintptr_t)tcg_ctx->code_gen_ptr + code_prefix + copy_size >=
            (uintptr_t)tcg_ctx->code_gen_highwater &&
            tcg_region_alloc(tcg_ctx)) {
        aot_request_tb_flush(cpu, mtcc_id, phys_pc);
    }

    tb = tcg_tb_alloc(tcg_ctx);
    if (unlikely(!tb)) {
        aot_request_tb_flush(cpu, mtcc_id, phys_pc);
    }

    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->pc = pc;
    tb->cs_base = cs_base;
#ifdef CONFIG_LATX_TU
    tu_reset_tb(tb);
#endif
    recover_tb_head(tb, file_tb);
    tb->tc.ptr = (void *)file_disp;
    tb_set_page_addr0(tb, phys_pc);
    tb_next_data_reset(tb);

    if (option_fast_itlb) {
        tb->hva = phys_pc & TARGET_PAGE_MASK;
    } else {
#ifdef CONFIG_SOFTMMU
        int error = 0;
        tb->hva = latxs_tr_cvt_cpu_addr_code(env, pc, &error);
#else
        tb->hva = 0;
#endif
    }
    tb_set_page_addr1(tb, -1);

#ifdef CONFIG_LATX_PROFILER
    CLN_TB_PROFILE(tb);
#endif
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    tcg_tb_insert(tb);

#ifdef CONFIG_LATX_PRINT
    if (tb->flags & HF_CPL_MASK) {
        tb_aot_cnt[cpu->cpu_index + 4]++;
    } else {
        tb_aot_cnt[cpu->cpu_index]++;
    }
#endif

    tb->tc.ptr = (void *)tcg_splitwx_to_rx(gen_code_buf);

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        uint64_t *tbm = (uint64_t *)gen_code_buf;
        tbmini_set_pointer(tbm, (uint64_t)tb);
        gen_code_buf = (tcg_insn_unit *)((uintptr_t)gen_code_buf + sizeof(struct TBMini));
        tb->tc.ptr = (void *)tcg_splitwx_to_rx(gen_code_buf);
    }
#endif

    uint32_t magic = *(uint32_t *)file_disp;
    if (magic != AOT_TB_MAGIC) {
        fprintf(stderr, "error tb magic %x in register_and_activate_tb\n", magic);
        _exit(0);
    }

    tb->misc_flags &= ~MF_FLAGS_NOEFLAGS;
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    set_mask_to_miscflags(tb, curr_mtcc(env),
            MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT);

    uint32_t all_tb_size = *(uint32_t *)(file_disp + 8);
    aot_block *a_file_tb = (aot_block *)(file_disp + 16);
    aot_rel *curr_tb_rel_table =
        (aot_rel *)((void *)a_file_tb + all_tb_size);

    void *aot_ir2_code = (void *)((uintptr_t)a_file_tb + sizeof(aot_block));

    if (!option_split_search) {
        search_size = tb->search_size;
    } else {
        search_size = 0;
        if (!region_alloc_search(tb, mtcc_id)) {
            aot_request_tb_flush(cpu, mtcc_id, phys_pc);
        }
        memcpy((void *)tb->search_ptr, aot_ir2_code + (tb->ir2_num << 2),
                tb->icount << 2);
    }

    if (option_pushpop) {
        if (!region_alloc_push_pop_data(tb, mtcc_id, tb->icount)) {
            aot_request_tb_flush(cpu, mtcc_id, phys_pc);
        }
        memcpy(tb_opt_ptr(tb),
                aot_ir2_code + (tb->ir2_num << 2) + (tb->icount << 2),
                tb->icount << 2);
    }

    if (tb->tc.ptr + copy_size >= tcg_ctx->code_gen_highwater) {
        aot_request_tb_flush(cpu, mtcc_id, phys_pc);
    }

    memcpy((void *)tb->tc.ptr, aot_ir2_code, copy_size);
    aot_do_tb_reloc(lsenv, tb, curr_tb_rel_table, rel_count);

    tb->preorder_tb_offset = 0;
    tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
    tb->ras_dest = (uintptr_t)NULL;
#endif
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 0);
        } else {
            tb_reset_jump(tb, 0);
        }
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 1);
        } else {
            tb_reset_jump(tb, 1);
        }
    }

    {
        ADDR s = (ADDR)tb->tc.ptr;
        ADDR e = s + gen_code_size;
        flush_idcache_range((uintptr_t)s, (uintptr_t)s, (uintptr_t)e);
    }

#if defined(CONFIG_LATX_TCG_MODE) || defined(CONFIG_TCG_REGION_TREE_ENABLE)
    tb->tc.size = gen_code_size;
#endif

    uintptr_t sptr = (uintptr_t)gen_code_buf + gen_code_size;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        sptr = sptr + sizeof(struct TBMini);
    }
#endif
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
        ROUND_UP(sptr + search_size, CODE_GEN_ALIGN));

    tb->cflags &= ~(CF_NO_ACTIVE | CF_INVALID);
    existing_tb = aot_tb_link_page(tb, false);

    if (unlikely(existing_tb != tb)) {
        uintptr_t orig_aligned = (uintptr_t)gen_code_buf;
        if (!option_split_tb) {
            orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
        }
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
        tcg_tb_remove(tb);
        return existing_tb;
    }

#if DEBUG_AOT
    register_tb_count++;
    load_core_tb_count++;
#endif
    return tb;
}

static void aot_link_page_ras(CPUState *cpu, uint64_t cs_base,
        uint32_t flags, int cflags, vaddr page_pc, int page_tb_count,
        uint64_t file_disp)
{
    while (page_tb_count--) {
        uint32_t magic = *(uint32_t *)file_disp;
        if (magic != AOT_TB_MAGIC) {
            fprintf(stderr, "aot_link_page_ras error tb magic %x\n", magic);
            return;
        }

        uint32_t sum_size = *(uint32_t *)(file_disp + 4);
        aot_block *file_tb = (aot_block *)(file_disp + 16);
        vaddr tb_pc = page_pc + (file_tb->pc & ~TARGET_PAGE_MASK);
        uint32_t clean_cflags = file_tb->cflags &
                ~(CF_NO_ACTIVE | CF_INVALID);

        if (file_tb->cs_base == cs_base && file_tb->flags == flags &&
                clean_cflags == cflags) {
            TranslationBlock *tb = aot_tb_lookup(cpu, tb_pc,
                    cs_base, flags, clean_cflags);

            if (tb && tb->inst_ptr_offset &&
                    ((tb->pc & TARGET_PAGE_MASK) ==
                     (tb->s_data->x86_next_pc & TARGET_PAGE_MASK))) {
                TranslationBlock *next_tb = aot_tb_lookup(cpu,
                        tb->s_data->x86_next_pc, cs_base, flags, clean_cflags);
                if (next_tb && !latxs_ras_link(tb, next_tb)) {
                    latxs_ras_unlink(tb);
                }
            }
        }

        file_disp += sum_size;
    }
}

static TranslationBlock *register_page_tb(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags, vaddr hva,
        int page_tb_count, uint64_t file_disp, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    bool perf_enabled = aot_perf_enabled();
    uint64_t perf_begin = perf_enabled ? aot_perf_now_ns() : 0;
    uint32_t original_page_tb_count = page_tb_count > 0 ? page_tb_count : 0;
    uint32_t registered_activated_count = 0;
    uint32_t aot_mode = (flags & HF_CPL_MASK) ? AOT_USER : AOT_CORE;

    TranslationBlock *ret_tb = NULL;
    uint32_t magic, all_tb_size, rel_count, offset, sum_size;
    vaddr page_pc = pc & TARGET_PAGE_MASK;
    uint64_t page_file_disp = file_disp;
    TranslationBlock *curr_tb;
    aot_rel *curr_tb_rel_table;

    /* bool not_can_do_hamt = !can_do_hamt(lsenv->cpu_state); */
    while (page_tb_count--) {
#if DEBUG_AOT
        uint64_t begin2 = get_tsc();
#endif
        magic = *(uint32_t *)file_disp;
        if (magic != AOT_TB_MAGIC) {
            fprintf(stderr, "register_page_tb error tb magic %x\n", magic);
            _exit(0);
        }
        sum_size = *(uint32_t *)(file_disp + 4);
        all_tb_size = *(uint32_t *)(file_disp + 8);
        rel_count = *(uint32_t *)(file_disp + 12);
        aot_block *curr_file_tb = (aot_block *)(file_disp + 16);

        /* if ( */
        /*         !(curr_file_tb->bool_flags & CAN_DO_HAMT) || */
        /*         (not_can_do_hamt != !(curr_file_tb->bool_flags & CAN_DO_HAMT)) */
        /*    ) { */
        /*     fprintf(stderr, "------------- not_can_do_hamt %d, curr_file_tb %d\n", not_can_do_hamt, */
        /*             (curr_file_tb->bool_flags & CAN_DO_HAMT)); */
        /*     file_disp += sum_size; */
        /*     continue; */
        /* } */

        curr_tb_rel_table =
            (aot_rel *)((void *)curr_file_tb + all_tb_size);
        offset = curr_file_tb->pc & ~TARGET_PAGE_MASK;

        if (curr_file_tb->cs_base != cs_base ||
                curr_file_tb->flags != flags ||
                (curr_file_tb->cflags & ~(CF_NO_ACTIVE | CF_INVALID)) !=
                    cflags) {
            file_disp += sum_size;
            continue;
        }

        if (!test_tb_reloc(curr_tb_rel_table, rel_count, page_pc + offset)) {
            file_disp += sum_size;
            continue;
        }

        TranslationBlock *existing_tb = aot_tb_lookup(cpu,
                page_pc + offset, cs_base, curr_file_tb->flags,
                curr_file_tb->cflags & ~(CF_INVALID | CF_NO_ACTIVE));
#if DEBUG_AOT
        uint64_t begin3 = get_tsc();
        t1 += begin3 - begin2;
#endif
        if (existing_tb) {
            if (existing_tb->pc == pc) {
                ret_tb = existing_tb;
            }
            file_disp += sum_size;
            continue;
        }
        curr_tb = register_and_activate_tb(pc, cpu, page_pc + offset, cs_base,
                    curr_file_tb, curr_tb_rel_table, rel_count, lsenv, file_disp);
#if DEBUG_AOT
        t2 += get_tsc() - begin3;
#endif
        if (!curr_tb) {
            fprintf(stderr, "[AOT] Load: stop registering page after TB recover failure\n");
            break;
        }
        registered_activated_count++;
        if (curr_tb->pc == pc
                && curr_tb->flags == flags
                && (curr_tb->cflags & ~(CF_NO_ACTIVE | CF_INVALID)) == cflags) {
            ret_tb = curr_tb;
        }
        file_disp += sum_size;
    }

    if (option_soft_ras) {
        aot_link_page_ras(cpu, cs_base, flags, cflags, page_pc,
                original_page_tb_count, page_file_disp);
    }

#if DEBUG_AOT
    register_page_tb_time += get_tsc() - begin;
#endif
    if (perf_enabled) {
        aot_perf_record_register_activate_tb(aot_mode, original_page_tb_count,
                registered_activated_count, aot_perf_now_ns() - perf_begin);
    }
    return ret_tb;
}

TranslationBlock *register_page(CPUState *cpu, vaddr pc, uint64_t cs_base, uint32_t flags,
        int cflags, ENV *lsenv, uint64_t phys_pc)
{

#ifdef CONFIG_SOFTMMU
    CPUArchState *env = cpu->env_ptr;
    int error = 0;
    ADDR hva = latxs_tr_cvt_cpu_addr_code(env, pc & TARGET_PAGE_MASK, &error);
    assert(!error);
#else
    ADDR hva = 1;
#endif
    TranslationBlock *tb = NULL;
#ifdef CONFIG_SOFTMMU
    uint64_t aot_mesage __attribute__((unused)) = get_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS);
#else
    uint64_t aot_mesage __attribute__((unused)) = 0;
#endif
    uint64_t file_addr = find_page(hva);
    if (file_addr) {
        page_head *ph = (page_head *)file_addr;
        tb = register_page_tb(cpu, pc, cs_base, flags, cflags,
                hva, ph->tb_count, file_addr + sizeof(page_head) + TARGET_PAGE_SIZE, lsenv);
#ifdef CONFIG_SOFTMMU
        set_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS, PAGE_LOADED, pc);
    } else {
        set_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS, PAGE_NO_FIND, pc);
#endif
    }

    return tb;
}

TranslationBlock *load_aot_tb(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    CPUArchState *env = cpu->env_ptr;

    void *host_pc;
    uint64_t phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);
    if (phys_pc == -1) {
        return NULL;
    }

#ifdef CONFIG_SOFTMMU
    if (!aot_host_pc_in_main_ram(host_pc)) {
        return NULL;
    }
#endif

    tb_lock_page0(phys_pc);

#ifdef CONFIG_SOFTMMU
#ifdef CONFIG_LATX_HACK_MEM
    if (!hamt_ptwflags_smc_page_is_protected(phys_pc)) {
        tlb_protect_code(phys_pc);
    }
#else
    tlb_protect_code(phys_pc);
#endif
#endif

    TranslationBlock *tb = NULL;

#ifdef CONFIG_SOFTMMU
    uint64_t aot_mesage = get_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS);
#else
    uint64_t aot_mesage = 0;
#endif

    /* fprintf(stderr, "load_aot_tb page %lx pc %lx cflags %lx flags %lx cs_base %lx\n", */
    /*         phys_pc & TARGET_PAGE_MASK, pc, cflags, flags, cs_base); */

    if ((aot_mesage & 0xf) == PAGE_NO_LOAD) {
        tb = register_page(cpu, pc, cs_base, flags, cflags, lsenv, phys_pc);
    }
    else if ((aot_mesage & 0xf) == PAGE_LOADED) {
        tb = aot_tb_lookup(cpu, pc, cs_base, flags, cflags | CF_INVALID | CF_NO_ACTIVE);
        if (!tb) {
            tb = register_page(cpu, pc, cs_base, flags, cflags, lsenv, phys_pc);
        }
    } else {
        /* unlock_page(phys_pc); */
        /* load_aot_time += (get_tsc() - begin); */
        /* return NULL; */
    }

    if (tb && (tb->cflags & CF_NO_ACTIVE)) {
        tb = activate_tb(tb, cpu, lsenv);
    } else {
#if DEBUG_AOT
        mis_tb++;
#endif
    }

#ifdef CONFIG_SOFTMMU
    unlock_page(phys_pc);
#endif
#if DEBUG_AOT
    load_aot_time += (get_tsc() - begin);
#endif

    return tb;
}

#ifdef CONFIG_LATX_TU
static inline aot_tu_page *aot_tu_pages(aot_tu_head *tu_head)
{
    return (aot_tu_page *)(tu_head + 1);
}

static inline aot_block *aot_tu_blocks(aot_tu_head *tu_head)
{
    return (aot_block *)(aot_tu_pages(tu_head) + tu_head->page_count);
}

typedef struct AOTTUStaticSummary {
    uintptr_t tu;
    uint32_t file_generation;
    uint32_t native_count, native_core_count, native_direct_count;
    uint32_t native_linked_count, native_indirect_count, native_osr_count;
    vaddr anchor_page;
    bool relocs_valid;
} AOTTUStaticSummary;

static __thread AOTTUStaticSummary aot_tu_static_cache;

/* Old mmap addresses are keys only. Return a value, not a TLS slot pointer
 * that a later load or nonlocal exit could replace. */
static AOTTUStaticSummary aot_tu_static_summary(aot_tu_head *tu_head,
                                              vaddr anchor_page)
{
    if (aot_tu_static_cache.file_generation != aot_file_generation ||
            aot_tu_static_cache.tu != (uintptr_t)tu_head) {
        aot_block *blocks = aot_tu_blocks(tu_head);
        uint64_t code_size = tu_head->tb_count ? blocks[0].tu_size : 0;
        const uint8_t *ir2 = (const uint8_t *)(blocks + tu_head->tb_count);
        const aot_rel *rels = (const void *)(ir2 + code_size);
        AOTTUStaticSummary summary = {
            .tu = (uintptr_t)tu_head,
            .file_generation = aot_file_generation,
            .anchor_page = anchor_page,
        };

        for (unsigned i = 0; i < tu_head->rel_count; i++) {
            summary.native_core_count += rels[i].kind == AOT_NATIVE_CORE;
            summary.native_direct_count +=
                rels[i].kind == AOT_NATIVE_DIRECT_CALL;
            summary.native_linked_count +=
                rels[i].kind == AOT_NATIVE_LINKED_CALL;
            summary.native_indirect_count +=
                rels[i].kind == AOT_NATIVE_INDIRECT_CALL;
            summary.native_count += rels[i].kind == AOT_NATIVE_ENTRY ||
                rels[i].kind == AOT_NATIVE_CALL ||
                rels[i].kind == AOT_NATIVE_CORE ||
                rels[i].kind == AOT_NATIVE_CORE_CALL ||
                rels[i].kind == AOT_NATIVE_DIRECT_CALL ||
                rels[i].kind == AOT_NATIVE_LINKED_CALL ||
                rels[i].kind == AOT_NATIVE_INDIRECT_CALL;
        }
        if (summary.native_core_count) {
            for (uint32_t b = 0; b < tu_head->tb_count; b++) {
                const aot_block *block = &blocks[b];
                if (block->rel_start < 0 || block->rel_end < block->rel_start ||
                    (uint32_t)block->rel_end >= tu_head->rel_count) {
                    continue;
                }
                uint64_t base = b ? block->tc.offset_in_tu : 0;
                if (base > code_size) {
                    continue;
                }
                for (int j = block->rel_start; j <= block->rel_end; j++) {
                    const aot_rel *rel = &rels[j];
                    if (rel->kind != AOT_NATIVE_CORE ||
                        rel->tc_offset > code_size - base) {
                        continue;
                    }
                    uint64_t at = base + rel->tc_offset;
                    uint64_t bytes = (uint64_t)rel->rel_slots_num * 4;
                    if (bytes > code_size - at) {
                        continue;
                    }
                    bool recognized = false;
                    const NativeRecoverHeader *h = native_recover_parse(
                        (const uint32_t *)(ir2 + at), bytes, &recognized);
                    if (h && !h->state_bytes && !h->points &&
                        h->osr_count <= NATIVE_RECOVER_MAX_OSR) {
                        summary.native_osr_count += h->osr_count;
                    }
                }
            }
        }
        aot_tu_static_cache = summary;
    }
    if (aot_tu_static_cache.anchor_page != anchor_page) {
        aot_tu_static_cache.anchor_page = anchor_page;
        aot_tu_static_cache.relocs_valid = false;
    }
    return aot_tu_static_cache;
}

static inline vaddr aot_tu_rebase_pc(const aot_tu_head *tu_head,
                                     vaddr anchor_page, vaddr saved_pc)
{
    vaddr saved_page = tu_head->first_pc & TARGET_PAGE_MASK;

    return anchor_page + (saved_pc - saved_page);
}

static int aot_phys_page_cmp(const void *ap, const void *bp)
{
    tb_page_addr_t a = *(const tb_page_addr_t *)ap;
    tb_page_addr_t b = *(const tb_page_addr_t *)bp;

    return a < b ? -1 : a > b;
}

/* Record bounds and cross-page reference uniqueness were checked at open. */
static bool aot_page_contains_tu_dependency(const page_head *page,
        const aot_tu_head *tu, int64_t pc_delta)
{
    const uint8_t *record = (const uint8_t *)(page + 1) + TARGET_PAGE_SIZE;

    for (uint32_t i = 0; i < page->tb_count; i++) {
        uint32_t magic = *(const uint32_t *)record;

        if (magic == AOT_TB_MAGIC) {
            const aot_tu_head *payload = (const aot_tu_head *)record;

            /* Single-page TU IDs may repeat: match the exact payload. */
            if (!pc_delta && payload == tu) {
                return true;
            }
            record += payload->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            const aot_tu_ref *ref = (const aot_tu_ref *)record;

            if (pc_delta && ref->tu_id == tu->tu_id &&
                    ref->anchor_delta == -pc_delta) {
                return true;
            }
            record += sizeof(*ref);
        } else {
            return false;
        }
    }
    return false;
}

static bool aot_tu_dependency_matches(const void *host_page,
        const aot_tu_head *tu, int64_t pc_delta)
{
    uint32_t h = get_page_hash((uint32_t *)host_page);
    uint32_t idx = h & (aot_hash_size - 1);

    /*
     * All dependency pages are locked and write-protected. The sampled hash
     * only selects candidates; compare every byte against this TU's saved
     * page. Continue past equal-content records belonging to other TUs.
     */
    for (uint32_t i = 0; i < aot_hash_size; i++) {
        uint64_t offset = hash_table[idx].offset;

        if (offset == UINT64_MAX) {
            break;
        }
        const page_head *page = (const page_head *)((const uint8_t *)
                in_tb_ptr + offset);

        if ((option_arch_opt ? hash_table[idx].hash : page->page_hash) == h &&
                aot_page_contains_tu_dependency(page, tu, pc_delta) &&
                !memcmp(host_page, page + 1, TARGET_PAGE_SIZE)) {
            return true;
        }
        idx = (idx + 1) & (aot_hash_size - 1);
    }
    return false;
}

static bool aot_lock_tu_dependencies(CPUState *cpu, vaddr anchor_page,
        aot_tu_head *tu_head, const tb_page_addr_t *extra_pages,
        unsigned extra_count)
{
    AOTTUPageDependency *deps;
    tb_page_addr_t *phys_pages = NULL;
    CPUArchState *env = cpu->env_ptr;
    aot_tu_page *pages = aot_tu_pages(tu_head);
    int deps_num = tu_head->page_count;
    int phys_num = 0;
    bool ok = false;

    g_assert(aot_tu_locked_pages_num == 0);
    g_assert(aot_tu_dependencies_num == 0);
    if (deps_num <= 0 || deps_num > AOT_TU_MAX_PAGES) {
        return false;
    }
    deps = g_try_new(AOTTUPageDependency, deps_num);
    phys_pages = g_try_new(tb_page_addr_t, (size_t)deps_num + extra_count);
    if (!deps || !phys_pages) {
        goto out;
    }
    for (int i = 0; i < deps_num; i++) {
        deps[i] = (AOTTUPageDependency) {
            .vp = anchor_page + pages[i].pc_delta,
            .pp = -1,
            .pc_delta = pages[i].pc_delta,
        };
    }

    for (int retry = 0; retry < 8; retry++) {
        bool stable = true;

        phys_num = 0;
        for (int i = 0; i < deps_num; i++) {
            void *host_page = NULL;
            tb_page_addr_t pp = aot_probe_code_page(env, deps[i].vp,
                                                   &host_page);
            if (pp == (tb_page_addr_t)-1 ||
                    !aot_host_pc_in_main_ram(host_page)) {
                goto out;
            }
            deps[i].pp = pp & TARGET_PAGE_MASK;
            phys_pages[phys_num++] = deps[i].pp;
#ifdef CONFIG_SOFTMMU
#ifdef CONFIG_LATX_HACK_MEM
            if (!tlb_code_page_is_protected(deps[i].pp)) {
                tlb_protect_code(deps[i].pp);
            }
#else
            /*
             * The protection query is a stub without HACK_MEM. Protect every
             * dependency before comparing its bytes and registering TBs.
             */
            tlb_protect_code(deps[i].pp);
#endif
#endif
        }

        for (unsigned i = 0; i < extra_count; i++) {
            phys_pages[phys_num++] = extra_pages[i];
        }
        qsort(phys_pages, phys_num, sizeof(*phys_pages), aot_phys_page_cmp);
        int unique_num = 0;
        for (int i = 0; i < phys_num; i++) {
            if (!unique_num || phys_pages[i] != phys_pages[unique_num - 1]) {
                phys_pages[unique_num++] = phys_pages[i];
            }
        }
        phys_num = unique_num;
        for (int i = 0; i < phys_num; i++) {
            tb_lock_page0(phys_pages[i]);
        }

        for (int i = 0; i < deps_num; i++) {
            void *host_page = NULL;
            tb_page_addr_t pp = aot_probe_code_page(
                    env, deps[i].vp, &host_page);

            if (pp == (tb_page_addr_t)-1 ||
                    (pp & TARGET_PAGE_MASK) != deps[i].pp
#ifdef CONFIG_SOFTMMU
                    || !tlb_code_page_is_protected(deps[i].pp)
#endif
                    ) {
                stable = false;
                break;
            }
            if (!aot_tu_dependency_matches(host_page, tu_head,
                                            deps[i].pc_delta)) {
                /*
                 * Stable mapping but wrong content: this candidate cannot
                 * be used. Do not repeat identical checks eight times.
                 */
                for (int j = phys_num - 1; j >= 0; j--) {
                    tb_unlock_page0(phys_pages[j]);
                }
                goto out;
            }
            deps[i].host_page = host_page;
        }
        if (stable) {
            memcpy(aot_tu_dependencies, deps, deps_num * sizeof(*deps));
            aot_tu_dependencies_num = deps_num;
            aot_tu_locked_pages = phys_pages;
            aot_tu_locked_pages_num = phys_num;
            phys_pages = NULL;
            ok = true;
            goto out;
        }
        for (int i = phys_num - 1; i >= 0; i--) {
            tb_unlock_page0(phys_pages[i]);
        }
    }

out:
    g_free(phys_pages);
    g_free(deps);
    return ok;
}

/* Consume only the mapping snapshot validated while all page locks are held. */
static tb_page_addr_t aot_locked_tu_page(vaddr pc, void **hostp)
{
    vaddr vp = pc & TARGET_PAGE_MASK;
    vaddr offset = pc & ~TARGET_PAGE_MASK;

    g_assert(aot_tu_locked_pages_num > 0);
    for (int i = 0; i < aot_tu_dependencies_num; i++) {
        const AOTTUPageDependency *dep = &aot_tu_dependencies[i];

        if (dep->vp == vp) {
            if (hostp) {
                *hostp = (char *)dep->host_page + offset;
            }
            return dep->pp + offset;
        }
    }
    return -1;
}

/* Only a live ordinary owner with the exact source mapping may relinquish
 * its key. AOT/native and temporary execution modes retain the old fallback. */
static bool aot_plain_conflict(CPUState *cpu, TranslationBlock *tb)
{
#ifdef CONFIG_SOFTMMU
    return tb && tb->tc.ptr && tcg_tb_has_private_s_data(tb) &&
        tb->size && tb->icount &&
        !(tb->bool_flags & IS_AOT_TB) && !tb_is_lookup_private(tb) &&
        !(tb_cflags(tb) & (CF_INVALID | CF_NO_ACTIVE | CF_PCREL |
                          CF_SINGLE_STEP | CF_USE_ICOUNT | CF_NO_GOTO_TB |
                          CF_NO_GOTO_PTR | CF_NOIRQ | CF_MEMI_ONLY)) &&
        get_mask_from_miscflags(tb, MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT) ==
            curr_mtcc(cpu->env_ptr) &&
        (!TB_TU_PAGES(tb) || !TB_TU_PAGES(tb)->native) &&
        tu_jmp_cache_can_insert(cpu, tb);
#else
    return false;
#endif
}

static void aot_conflict_add_page(GArray *pages, tb_page_addr_t page)
{
    if (page == (tb_page_addr_t)-1) {
        return;
    }
    page &= TARGET_PAGE_MASK;
    for (unsigned i = 0; i < pages->len; i++) {
        if (g_array_index(pages, tb_page_addr_t, i) == page) {
            return;
        }
    }
    g_array_append_val(pages, page);
}

static void aot_conflict_pages(GArray *pages, const TranslationBlock *tb)
{
    aot_conflict_add_page(pages, tb_page_addr0(tb));
    aot_conflict_add_page(pages, tb_page_addr1(tb));
    const TranslationUnitPages *sources = TB_TU_PAGES(tb);
    if (sources) {
        for (unsigned i = 0; i < TRANSLATION_UNIT_MAX_PAGES + 1; i++) {
            uint64_t packed = sources->pages[i].vp_phys_hi;
            if (packed == UINT64_MAX) {
                break;
            }
            aot_conflict_add_page(pages,
                tu_page_desc_phys(packed, sources->pages[i].mix));
        }
    }
}

static bool aot_conflict_pages_locked(const TranslationBlock *tb)
{
#ifdef CONFIG_SOFTMMU
    GArray *pages = g_array_new(false, false, sizeof(tb_page_addr_t));
    bool locked = true;
    aot_conflict_pages(pages, tb);
    for (unsigned i = 0; i < pages->len; i++) {
        tb_page_addr_t page = g_array_index(pages, tb_page_addr_t, i);
        bool found = false;
        /* Check this thread's lock set, not merely whether any CPU owns it. */
        for (int j = 0; j < aot_tu_locked_pages_num; j++) {
            found |= aot_tu_locked_pages[j] == page;
        }
        locked &= found;
    }
    g_array_free(pages, true);
    return locked;
#else
    return false;
#endif
}

static TranslationUnitPages *aot_build_tu_pages_desc(
        const aot_tu_head *tu_head, vaddr anchor_page, TCGContext *ctx)
{
    const aot_tu_page *pages = aot_tu_pages((aot_tu_head *)tu_head);
    TranslationUnitPages *desc = region_alloc_tu_pages(ctx);

    if (!desc) {
        return NULL;
    }
    memset(desc, 0xff, sizeof(*desc));
    desc->id = tu_pages_alloc_id();
    desc->hras_members = NULL;
    desc->hras_member_count = 0;
    desc->native = NULL;
    for (uint32_t i = 0; i < tu_head->page_count; i++) {
        vaddr vp = anchor_page + pages[i].pc_delta;
        tb_page_addr_t pp = aot_locked_tu_page(vp, NULL);

        if (pp == (tb_page_addr_t)-1) {
            return NULL;
        }
        pp &= TARGET_PAGE_MASK;
        desc->pages[i].vp_phys_hi = tu_page_desc_pack_vp(vp, pp);
        desc->pages[i].mix = tb_cal_page_addr_mix(vp, pp);
    }
    return desc;
}

TranslationBlock *load_tu(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags,
        vaddr hva, uint64_t file_disp, vaddr anchor_page, ENV *lsenv,
        uint32_t *loaded_tb_count)
{
    aot_tu_head *tu_head = (aot_tu_head *)file_disp;
    if (tu_head->tb_magic != AOT_TB_MAGIC) {
        fprintf(stderr, "error tu magic %x in load_tu\n", tu_head->tb_magic);
        _exit(0);
    }
    int tb_num_in_tu = tu_head->tb_count;
    uint32_t loaded_count = 0;

    if (loaded_tb_count) {
        *loaded_tb_count = 0;
    }

    TranslationBlock *tb;
    aot_block *aot_tb;
    TranslationBlock *ret = NULL;
    aot_block *file_tb_vector = aot_tu_blocks(tu_head);
    int ir2_size = file_tb_vector[0].tu_size;
    aot_rel *curr_rel_table =
        (aot_rel *)((uintptr_t)(file_tb_vector + tu_head->tb_count) + ir2_size);
    uintptr_t search_code_pos = (uintptr_t)curr_rel_table + tu_head->rel_count * sizeof(aot_rel);

    void *host_pc;
    tb_page_addr_t phys_pc;
    tb_page_addr_t locked_phys_pc;
    CPUArchState *env = cpu->env_ptr;
    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    assert_memory_lock();
    qemu_thread_jit_write();
    locked_phys_pc = aot_locked_tu_page(pc, NULL);
    if (locked_phys_pc == (tb_page_addr_t)-1) {
        return NULL;
    }

    AOTTUStaticSummary summary = aot_tu_static_summary(tu_head, anchor_page);
    uint32_t native_count = summary.native_count;
    uint32_t native_core_count = summary.native_core_count;
    uint32_t native_direct_count = summary.native_direct_count;
    bool native_core = native_core_count != 0;
    /* Native recovery needs LASX; LLVM uses the optimal PAUSE/RDTSC paths. */
    if (native_core && (!(cpuinfo & CPUINFO_LASX) ||
                        !option_pause || !option_opt_rdtsc_rdtscp)) {
        return NULL;
    }
    if (!native_core) {
        int saved_error_code = env->error_code;
        TranslationBlock *existing = tb_htable_lookup_noexit(cpu, pc,
                cs_base, flags, cflags);
        env->error_code = saved_error_code;
        if (existing) {
            /* File validation proved member-page coverage. All dependencies
             * are locked and their mappings/bytes rechecked on every call.
             * Only relocation ranges need a per-anchor static check. */
            if (!summary.relocs_valid) {
                for (int i = 0; i < tb_num_in_tu; i++) {
                    aot_block *member = &file_tb_vector[i];
                    vaddr curr_pc = aot_tu_rebase_pc(tu_head, anchor_page,
                                                    member->pc);
                    uint32_t count = member->rel_start < 0 ? 0 :
                        member->rel_end - member->rel_start + 1;
                    if (count && !test_tb_reloc(
                            curr_rel_table + member->rel_start, count, curr_pc)) {
                        return NULL;
                    }
                }
                if (aot_file_generation == summary.file_generation &&
                        aot_tu_static_cache.file_generation ==
                            summary.file_generation &&
                        aot_tu_static_cache.tu == summary.tu &&
                        aot_tu_static_cache.anchor_page == summary.anchor_page) {
                    aot_tu_static_cache.relocs_valid = true;
                }
            }
            return existing;
        }
    }
    /* Each invalidated LLVM body needs its own guest PC and cold entry. */
    size_t native_entry_cold_bytes =
        (size_t)native_core_count * AOT_CORE_COLD_BYTES +
        (size_t)native_direct_count * AOT_DIRECT_COLD_BYTES +
        (size_t)summary.native_indirect_count * AOT_INDIRECT_COLD_BYTES +
        (size_t)summary.native_linked_count * AOT_LINKED_COLD_BYTES +
        (size_t)summary.native_osr_count * AOT_OSR_STUB_BYTES;
#ifdef CONFIG_SOFTMMU
    size_t ordinary_cold_offset = native_entry_cold_bytes;
    for (int i = 0; i < tb_num_in_tu; i++) {
        if (aot_ordinary_call_exit(&file_tb_vector[i], curr_rel_table, env)) {
            native_entry_cold_bytes += AOT_DIRECT_RESOLVER_BYTES;
        }
    }
#endif
    size_t code_prefix = CODE_GEN_ALIGN - 1;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    code_prefix += sizeof(struct TBMini) * (tb_num_in_tu + 1);
    if (native_core) {
        code_prefix += CODE_GEN_ALIGN - 1;
    }
#endif
    if ((uintptr_t)tcg_ctx->code_gen_ptr + code_prefix + ir2_size +
            native_entry_cold_bytes >=
            (uintptr_t)tcg_ctx->code_gen_highwater &&
            tcg_region_alloc(tcg_ctx)) {
        aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
    }
    bool existing_tu = false;
    bool replace_plain = native_core;
    TranslationBlock **conflicts = NULL;
    if (native_core) {
        g_assert(aot_tu_conflicts == NULL);
        conflicts = g_try_new0(TranslationBlock *, tb_num_in_tu);
        if (!conflicts) {
            return (TranslationBlock *)-1;
        }
        aot_tu_conflicts = conflicts;
    }
    for (int i = 0; i < tb_num_in_tu; i++) {
        aot_tb = &file_tb_vector[i];
        vaddr curr_pc = aot_tu_rebase_pc(tu_head, anchor_page,
                                         aot_tb->pc);
        if (aot_locked_tu_page(curr_pc, NULL) == (tb_page_addr_t)-1 ||
                (aot_tb->page_addr[1] != (tb_page_addr_t)-1 &&
                 aot_locked_tu_page((curr_pc & TARGET_PAGE_MASK) +
                     TARGET_PAGE_SIZE, NULL) == (tb_page_addr_t)-1)) {
            return NULL;
        }
        uint32_t rel_count = aot_tb->rel_start < 0 ? 0 :
            aot_tb->rel_end - aot_tb->rel_start + 1;
        if (rel_count &&
                !test_tb_reloc(curr_rel_table + aot_tb->rel_start,
                    rel_count, curr_pc)) {
            return NULL;
        }
        /* if (not_can_do_hamt != !(aot_tb->bool_flags & CAN_DO_HAMT)) { */
        /*     fprintf(stderr, "can_do_hamt not equal curr %x aot %x\n", */
        /*             !not_can_do_hamt, aot_tb->bool_flags & CAN_DO_HAMT); */
        /*     return NULL; */
        /* } */
        int saved_error_code = env->error_code;
        TranslationBlock *existing = tb_htable_lookup_noexit(cpu, curr_pc,
                cs_base, aot_tb->flags, aot_tb->cflags);
        env->error_code = saved_error_code;
        if (existing) {
            existing_tu = true;
            if (native_core) {
                if (existing->bool_flags & IS_AOT_TB) {
                    aot_tu_blocked.tu = tu_head;
                    aot_tu_blocked.anchor_page = anchor_page;
                    aot_tu_blocked.member_pc = curr_pc;
                    aot_tu_blocked.cs_base = cs_base;
                    aot_tu_blocked.file_generation = aot_file_generation;
                    aot_tu_blocked.flags = aot_tb->flags;
                    aot_tu_blocked.cflags = aot_tb->cflags;
                }
                conflicts[i] = existing;
                replace_plain &= aot_plain_conflict(cpu, existing) &&
                    aot_conflict_pages_locked(existing) &&
                    existing->pc == curr_pc && existing->cs_base == cs_base &&
                    existing->flags == aot_tb->flags &&
                    tb_cflags(existing) == aot_tb->cflags &&
                    ((aot_tb->misc_flags & MF_MTCC_ID_MASK) >>
                     MF_MTCC_ID_SHIFT) == mtcc_id;
            }
            if (curr_pc == pc && aot_tb->flags == flags &&
                    aot_tb->cflags == cflags) {
                ret = existing;
            }
        }
    }
    lats_middle_cache.valid_checked = false;
    bool owner_local_returns = !native_count && aot_paired_ordinary_tu(
            file_tb_vector, tb_num_in_tu, curr_rel_table,
            (const uint8_t *)(file_tb_vector + tb_num_in_tu), ir2_size);
    if (existing_tu && !replace_plain && !owner_local_returns) {
        return ret;
    }

    TranslationUnitPages *tu_pages = NULL;
    if (tu_head->page_count > 1 || tucp_fast_needs_pages_desc() || native_count) {
        tu_pages = aot_build_tu_pages_desc(tu_head, anchor_page,
                                          tcg_ctx);
        if (!tu_pages) {
            aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
        }
    }

    int pre_search_size = 0;
    g_autofree TranslationBlock **curr_tb_vector =
        g_try_new0(TranslationBlock *, tu_head->tb_count);
    if (!curr_tb_vector) {
        return (TranslationBlock *)-1;
    }
    for (int i = 0; i < tb_num_in_tu; i++) {
        curr_tb_vector[i] = tcg_tb_alloc_full(tcg_ctx);
        tb = curr_tb_vector[i];
        if (unlikely(!tb)) {
            aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
        }
        tu_reset_tb(tb);
        vaddr curr_pc = aot_tu_rebase_pc(tu_head, anchor_page,
                                         file_tb_vector[i].pc);
        phys_pc = aot_locked_tu_page(curr_pc, &host_pc);
        g_assert(phys_pc != (tb_page_addr_t)-1);
        tb->pc = curr_pc;
        tb->cs_base = cs_base;

        recover_tb_head(tb, &file_tb_vector[i]);
        tb->s_data->aot_native_call_entry = NULL;
        tb->s_data->aot_direct_native_entry = NULL;
        TB_TU_PAGES(tb) = tu_pages;

        tb->misc_flags &= ~MF_FLAGS_NOEFLAGS;
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
        if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
            add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
        }
#endif
        /* set_mask_to_miscflags(tb, curr_mtcc(env), */
        /*         MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT); */

        tb_set_page_addr0(tb, phys_pc);
        tb_next_data_reset(tb);
        if (option_fast_itlb) {
            tb->hva = phys_pc & TARGET_PAGE_MASK;
        } else {
            tb->hva = (uintptr_t)host_pc;
        }
        if (file_tb_vector[i].page_addr[1] != -1) {
            vaddr page1_pc = (curr_pc & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
            tb_page_addr_t phys_page1 =
                aot_locked_tu_page(page1_pc, NULL);
            g_assert(phys_page1 != (tb_page_addr_t)-1);
            tb_set_page_addr1(tb, phys_page1);
        } else {
            tb_set_page_addr1(tb, -1);
        }
        /* latx_mmio_tb_check(tb); */
#ifdef CONFIG_LATX_PROFILER
        CLN_TB_PROFILE(tb);
#endif
        /* init jump list */
        qemu_spin_init(&tb->jmp_lock);
        tb->jmp_list_head = (uintptr_t)NULL;
        tb->jmp_list_next[0] = (uintptr_t)NULL;
        tb->jmp_list_next[1] = (uintptr_t)NULL;
        tb->jmp_dest[0] = (uintptr_t)NULL;
        tb->jmp_dest[1] = (uintptr_t)NULL;
        tb->s_data->is_invalidate = false;
    }

    uintptr_t tbmini_ptr = 0;
#if defined(CONFIG_TCG_REGION_TREE_ENABLE)
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
            ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN));
#else

    qatomic_set(&tcg_ctx->code_gen_ptr, ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN));

    tbmini_ptr = (uintptr_t)
                ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN);
    uintptr_t last_tbmini_ptr = tbmini_ptr +
                sizeof(struct TBMini) * (tb_num_in_tu + 1);
    qatomic_set(&tcg_ctx->code_gen_ptr, last_tbmini_ptr);
    /* The table header stores a member count rather than a TB pointer. */
    tu_tbmini_set_pointer((uint64_t *)tbmini_ptr, tb_num_in_tu, TU_TBM_MAGIC);
    uintptr_t curr_tbmini_ptr = (uintptr_t)tcg_ctx->code_gen_ptr -
                        sizeof(struct TBMini) * (tb_num_in_tu + 1);
    if (native_core) {
        /* Keep LLVM function and loop alignment after the lookup table. */
        qatomic_set(&tcg_ctx->code_gen_ptr,
                    ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN));
    }
#endif

    tcg_insn_unit *gen_code_buf = (tcg_insn_unit *)tcg_splitwx_to_rx(tcg_ctx->code_gen_ptr);
    tb = curr_tb_vector[0];
    tb->tc.ptr = gen_code_buf;
    if ((uintptr_t)gen_code_buf + ir2_size + native_entry_cold_bytes >=
            (uintptr_t)tcg_ctx->code_gen_highwater) {
        aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
    }
    void *aot_ir2_code = file_tb_vector + tu_head->tb_count;
    memcpy(gen_code_buf, aot_ir2_code, ir2_size);

    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
            ROUND_UP((uintptr_t)(gen_code_buf) + ir2_size + native_entry_cold_bytes,
                     CODE_GEN_ALIGN));
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    lats_register_tu_chunk_hint(tbmini_ptr, (uintptr_t)gen_code_buf,
                                (uintptr_t)gen_code_buf + ir2_size);
#endif

    for (int i = 0; i < tu_head->tb_count; i++) {
        tb = curr_tb_vector[i];

#ifdef CONFIG_SOFTMMU
        /* Validate the complete saved SYSCALL encoding before publishing
         * a native entry. Prefixes contribute to RCX's return address. */
        const aot_block *source = &file_tb_vector[i];
        if (source->rel_start >= 0) {
            for (int j = source->rel_start; j <= source->rel_end; j++) {
                const aot_rel *rel = &curr_rel_table[j];
                if (rel->kind != AOT_NATIVE_SYSCALL &&
                    rel->kind != AOT_NATIVE_ATOMIC &&
                    rel->kind != AOT_NATIVE_CPU_QUERY &&
                    rel->kind != AOT_NATIVE_X87 &&
                    rel->kind != AOT_NATIVE_SYSTEM)
                    continue;
                bool atomic = rel->kind == AOT_NATIVE_ATOMIC;
                bool query = rel->kind == AOT_NATIVE_CPU_QUERY;
                bool x87 = rel->kind == AOT_NATIVE_X87;
                bool system = rel->kind == AOT_NATIVE_SYSTEM;
                unsigned length =
                    system   ? LATS_SYSTEM_LENGTH(rel->extra_addend)
                    : atomic ? LATS_ATOMIC_LENGTH(rel->extra_addend)
                    : query  ? LATS_CPU_QUERY_LENGTH(rel->extra_addend)
                    : x87    ? LATS_X87_LENGTH(rel->extra_addend)
                             : rel->extra_addend;
                uint8_t actual[15];
                if (length < (x87 ? 1 : 2) || length > sizeof(actual) ||
                    length > tb->size ||
                    (tb->flags & (HF_LMA_MASK | HF_CS64_MASK | HF_CPL_MASK)) !=
                        (HF_LMA_MASK | HF_CS64_MASK | 3) ||
                    (x87 && !option_soft_fpu) ||
                    !(system   ? GET_SC_TABLE(mtcc_id, aot_native_system)
                      : x87    ? GET_SC_TABLE(mtcc_id, aot_native_x87)
                      : query  ? GET_SC_TABLE(mtcc_id, aot_native_cpu_query)
                      : atomic ? GET_SC_TABLE(mtcc_id, aot_native_atomic)
                               : GET_SC_TABLE(mtcc_id, aot_native_syscall)))
                    return NULL;
                for (unsigned k = 0; k < length; k++) {
                    void *host;
                    if (aot_locked_tu_page(tb->pc + tb->size - length + k, &host) ==
                        (tb_page_addr_t)-1) return NULL;
                    memcpy(actual + k, host, 1);
                }
                if (atomic) {
                    if (lats_aot_atomic_decode(actual, length) !=
                        (rel->extra_addend & ~(uint64_t)LATS_ATOMIC_RETURN)) {
                        return NULL;
                    }
                    continue;
                }
                if (query) {
                    if (lats_aot_cpu_query_decode(actual, length) != rel->extra_addend) return NULL;
                    continue;
                }
                if (system) {
                    if (lats_aot_system_decode(actual, length) !=
                        rel->extra_addend)
                        return NULL;
                    continue;
                }
                if (x87) {
                    if (lats_aot_x87_decode(actual, length) != rel->extra_addend) return NULL;
                    continue;
                }
                for (unsigned k = 0; k + 2 < length; k++) {
                    uint8_t prefix = actual[k];
                    if (prefix != 0x26 && prefix != 0x2e && prefix != 0x36 &&
                        prefix != 0x3e && prefix != 0x64 && prefix != 0x65 &&
                        prefix != 0x66 && prefix != 0x67 &&
                        (prefix < 0x40 || prefix > 0x4f)) return NULL;
                }
                if (actual[length - 2] != 0x0f || actual[length - 1] != 0x05)
                    return NULL;
            }
        }
#endif

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
        uint64_t size = tb->ir2_num << 2;
        curr_tbmini_ptr += sizeof(struct TBMini);
        tu_tbmini_set_pointer((uint64_t *)curr_tbmini_ptr, (uint64_t)tb, size);
#endif

        if (i) {
            tb->tc.ptr = (void *)((uintptr_t)gen_code_buf + tb->tc.offset_in_tu);
        }
#if defined(CONFIG_TCG_REGION_TREE_ENABLE)
        /* Address lookup needs each ordinary member's exact code range. */
        tb->tc.size = tb->ir2_num << 2;
#endif
        uint32_t rel_count = file_tb_vector[i].rel_start < 0 ? 0 :
            file_tb_vector[i].rel_end - file_tb_vector[i].rel_start + 1;
        if (rel_count) {
            aot_do_tb_reloc(lsenv, tb,
                    curr_rel_table + file_tb_vector[i].rel_start, rel_count);
        }
        if (!option_split_search) {
            fprintf(stderr, "no support !option_split_search\n");
            exit(0);
        } else {
            if (!region_alloc_search(tb, mtcc_id)) {
                aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
            }
            memcpy((void*)tb->search_ptr,
                    (void *)(search_code_pos + pre_search_size), tb->icount << 2);
            pre_search_size += tb->icount << 2;
        }
        tb->s_data->next_tb[0] = NULL;
        tb->s_data->next_tb[1] = NULL;
        if (i) {
            tb->s_data->next_tb[0] = curr_tb_vector[i - 1];
        }
        if (i != tu_head->tb_count - 1) {
            tb->s_data->next_tb[1] = curr_tb_vector[i + 1];
        }
        tb->bool_flags |= IS_TU_TB;
    }

#ifdef CONFIG_SOFTMMU
    aot_load_ordinary_call_hints(file_tb_vector, tb_num_in_tu,
            curr_rel_table, aot_ir2_code, (uint8_t *)gen_code_buf, ir2_size);
    for (int i = 0; i < tb_num_in_tu; i++) {
        if (!aot_paired_call_slot(file_tb_vector, tb_num_in_tu, i,
                curr_rel_table, aot_ir2_code, ir2_size)) continue;
        tb = curr_tb_vector[i];
        uint8_t actual[5] = { 0 };
        if (tb->size < sizeof(actual)) continue;
        bool readable = true;
        for (unsigned k = 0; k < sizeof(actual); k++) {
            void *host;
            if (aot_locked_tu_page(tb->pc + tb->size - 5 + k, &host) ==
                    (tb_page_addr_t)-1) {
                readable = false;
                break;
            }
            memcpy(actual + k, host, 1);
        }
        int32_t displacement;
        memcpy(&displacement, actual + 1, sizeof(displacement));
        if (readable && actual[0] == 0xe8 &&
            tb->pc + tb->size + displacement == tb->next_pc[0]) {
            tb->bool_flags |= IS_HRAS_CALL;
        }
    }
#endif
    flush_idcache_range((uintptr_t)gen_code_buf, (uintptr_t)gen_code_buf,
                        ir2_size);

#ifdef CONFIG_SOFTMMU
    for (int i = 0; i < tb_num_in_tu; i++) {
        aot_block *block = &file_tb_vector[i];
        const aot_rel *rel = aot_ordinary_call_exit(block, curr_rel_table, env);
        if (!rel) continue;
        uint32_t *resolver = (void *)((uintptr_t)gen_code_buf + ir2_size +
                                      ordinary_cold_offset);
        ordinary_cold_offset += AOT_DIRECT_RESOLVER_BYTES;
        tb = curr_tb_vector[i];
        uint8_t actual[5];
        for (unsigned k = 0; k < sizeof(actual); k++) {
            void *host;
            if (aot_locked_tu_page(tb->pc + tb->size - 5 + k, &host) ==
                    (tb_page_addr_t)-1) {
                return NULL;
            }
            memcpy(actual + k, host, 1);
        }
        int32_t displacement;
        memcpy(&displacement, actual + 1, sizeof(displacement));
        /* Do not reinterpret indirect CALLs or noncanonical source bytes. */
        if (actual[0] != 0xe8 ||
            tb->pc + tb->size + displacement != tb->next_pc[0]) continue;
        uint32_t *slot = (void *)((uintptr_t)tb->tc.ptr + rel->tc_offset);
        if (aot_native_call_resolver(resolver, slot, tb, tb->next_pc[0],
                                     mtcc_id, true)) {
            flush_idcache_range((uintptr_t)slot, (uintptr_t)slot, 4);
        }
    }
#endif

    if (native_count) {
        size_t bytes = sizeof(AOTNativeTUInfo) +
                       native_count * sizeof(AOTNativeRecord);
        AOTNativeTUInfo *info = tcg_region_alloc_data(tcg_ctx, bytes);
        if (!info) {
            aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
        }
        memset(info, 0, bytes);
#ifdef CONFIG_SOFTMMU
        uint32_t native_direct_index = 0;
        uint32_t native_indirect_index = 0;
        uint32_t native_linked_index = 0;
#endif
        for (int i = 0; i < tb_num_in_tu; i++) {
            aot_block *block = &file_tb_vector[i];
            if (block->rel_start < 0) {
                continue;
            }
            for (int j = block->rel_start; j <= block->rel_end; j++) {
                aot_rel *rel = &curr_rel_table[j];
                if (rel->kind != AOT_NATIVE_ENTRY &&
                    rel->kind != AOT_NATIVE_CALL &&
                    rel->kind != AOT_NATIVE_CORE &&
                    rel->kind != AOT_NATIVE_CORE_CALL &&
                    rel->kind != AOT_NATIVE_DIRECT_CALL &&
                    rel->kind != AOT_NATIVE_LINKED_CALL &&
                    rel->kind != AOT_NATIVE_INDIRECT_CALL) {
                    continue;
                }
                if (info->count >= native_count) {
                    tu_pages->native = NULL;
                    return NULL;
                }
                AOTNativeRecord *record = &info->records[info->count++];
                record->tb = curr_tb_vector[i];
                uint32_t *address = (void *)((uintptr_t)record->tb->tc.ptr +
                                           rel->tc_offset);
                if (rel->kind == AOT_NATIVE_ENTRY ||
                    rel->kind == AOT_NATIVE_CORE) {
                    record->entry = address;
                    record->entry_bytes = rel->rel_slots_num * 4;
                    bool recognized;
                    record->recover = native_recover_parse(address,
                            record->entry_bytes, &recognized);
                    /* A direct-register record has a CORE entry and may not
                     * use the legacy State-wrapper entry/linking path. */
                    const NativeRecoverHeader *recover = record->recover;
                    if (recover && !recover->state_bytes &&
                        rel->kind != AOT_NATIVE_CORE) {
                        return NULL;
                    }
                    if ((recognized && !record->recover) ||
                        (rel->kind == AOT_NATIVE_CORE &&
                         (!recognized || !record->recover ||
                          !can_do_hamt(cpu->env_ptr) || !hamt_mode_dmw()))) {
                        return NULL;
                    }
                    if (rel->kind == AOT_NATIVE_CORE) {
                        const NativeRecoverHeader *h = record->recover;
                        if (h->state_bytes) {
                            if (rel->next_entry_id < 0 ||
                                (rel->next_entry_id & 3) || h->code_bytes < 4 ||
                                (uint32_t)rel->next_entry_id >
                                    h->code_bytes - 4) {
                                return NULL;
                            }
                            record->core = (void *)((uintptr_t)address +
                                                   rel->next_entry_id);
                        } else {
                            if (h->code_bytes < 4 ||
                                (rel->next_entry_id != 0 &&
                                 (rel->next_entry_id != 32 ||
                                  h->code_bytes < 36))) {
                                return NULL;
                            }
                            record->core = address;
                            if (rel->next_entry_id == 32) {
                                record->direct_native_entry = address + 8;
                            }
                        }
                    }
                } else if (rel->kind == AOT_NATIVE_INDIRECT_CALL) {
#if !defined(CONFIG_SOFTMMU) || !defined(CONFIG_TB_FLAGS_NO_EFLAGS)
                    return NULL;
#else
                    CPUX86State *env = cpu->env_ptr;
                    uint32_t *poison = (void *)((uintptr_t)gen_code_buf +
                                               (uint32_t)rel->next_entry_id);
                    uintptr_t fallback = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
                    int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)poison;
                    int64_t offset = delta >> 2;
                    int64_t cont_delta = (intptr_t)poison - (intptr_t)(address + 1);
                    int64_t cont_offset = cont_delta >> 2;
                    if (!option_soft_ras || !option_rastoptp || !option_intb_njc ||
                        !latx_jmp_cache_compact_enabled() ||
                        latx_jmp_cache_hash_mode() != LATX_JMP_CACHE_HASH_PAGE ||
                        latx_jmp_cache_bits() != 12 || !env->latx_njc_cache_ptr ||
                        !can_do_hamt(env) || !hamt_mode_dmw() || !fallback ||
                        (delta & 3) || offset != sextract64(offset, 0, 36) ||
                        (cont_delta & 3) || cont_offset != sextract64(cont_offset, 0, 26)) {
                        return NULL;
                    }
                    poison[0] = 0x1e000004U |
                        ((((offset + (1 << 15)) >> 16) & 0xfffff) << 5);
                    poison[1] = 0x4c000080U | ((offset & 0xffff) << 10);
                    flush_idcache_range((uintptr_t)poison, (uintptr_t)poison, 8);
                    record->direct_continuation = address + 1;
                    record->direct_continuation_invalid = 0x50000000U |
                        ((cont_offset >> 16) & 0x3ff) |
                        (((uint64_t)cont_offset << 10) & 0x3fffc00);
                    uint32_t *resolver = (void *)((uintptr_t)gen_code_buf +
                        ir2_size + (size_t)native_core_count * AOT_CORE_COLD_BYTES +
                        (size_t)native_direct_count * AOT_DIRECT_COLD_BYTES +
                        (size_t)native_indirect_index++ * AOT_INDIRECT_COLD_BYTES);
                    if ((uintptr_t)address >= (uintptr_t)gen_code_buf + 36) {
                        aot_native_indirect_resolver(resolver, address,
                                record->tb, poison, mtcc_id);
                    }
#endif
                } else if (rel->kind == AOT_NATIVE_CORE_CALL ||
                           rel->kind == AOT_NATIVE_DIRECT_CALL ||
                           rel->kind == AOT_NATIVE_LINKED_CALL) {
                    uint8_t actual[5];
                    if (record->tb->size < sizeof(actual)) {
                        return NULL;
                    }
                    vaddr call_pc = record->tb->pc + record->tb->size -
                                    sizeof(actual);
                    for (unsigned k = 0; k < sizeof(actual); k++) {
                        void *host;
                        if (aot_locked_tu_page(call_pc + k, &host) ==
                            (tb_page_addr_t)-1) {
                            return NULL;
                        }
                        memcpy(actual + k, host, 1);
                    }
                    int32_t displacement;
                    memcpy(&displacement, actual + 1, sizeof(displacement));
                    if (actual[0] != 0xe8 || displacement !=
                        (int32_t)(rel->extra_addend >> 32)) {
                        return NULL;
                    }
                    record->callee_pc = call_pc + sizeof(actual) +
                                        displacement;
                    if (rel->kind == AOT_NATIVE_CORE_CALL) {
                        record->binding_slot = (void *)address;
                        record->binding_slot[0] = 0;
                        record->binding_slot[1] = DMW_IN((uintptr_t)gen_code_buf +
                                                  (uint32_t)rel->extra_addend);
                    } else {
                        uint32_t *poison = (void *)((uintptr_t)gen_code_buf +
                                                   (uint32_t)rel->next_entry_id);
                        uintptr_t fallback = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
                        int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)poison;
                        int64_t offset = delta >> 2;
                        int64_t cont_delta = (intptr_t)poison - (intptr_t)(address + 1);
                        int64_t cont_offset = cont_delta >> 2;
                        if (!option_soft_ras || !option_rastoptp ||
                            !tucp_generation_offset() ||
                            !can_do_hamt(cpu->env_ptr) || !hamt_mode_dmw() ||
                            !fallback || (delta & 3) ||
                            offset != sextract64(offset, 0, 36) ||
                            (cont_delta & 3) ||
                            cont_offset != sextract64(cont_offset, 0, 26)) {
                            return NULL;
                        }
                        poison[0] = 0x1e000004U |
                            ((((offset + (1 << 15)) >> 16) & 0xfffff) << 5);
                        poison[1] = 0x4c000080U | ((offset & 0xffff) << 10);
                        flush_idcache_range((uintptr_t)poison,
                                            (uintptr_t)poison, 8);
                        if (rel->kind == AOT_NATIVE_LINKED_CALL) {
#ifdef CONFIG_SOFTMMU
                            uint32_t *resolver = (void *)((uintptr_t)gen_code_buf +
                                ir2_size + (size_t)native_core_count * AOT_CORE_COLD_BYTES +
                                (size_t)native_direct_count * AOT_DIRECT_COLD_BYTES +
                                (size_t)summary.native_indirect_count *
                                    AOT_INDIRECT_COLD_BYTES +
                                (size_t)native_linked_index++ *
                                    AOT_LINKED_COLD_BYTES);
                            uint32_t cold_offset = rel->extra_addend;
                            if (ir2_size < 32 || cold_offset > ir2_size - 32) {
                                return NULL;
                            }
                            if (aot_native_linked_call_resolver(resolver,
                                    address, record->tb, record->callee_pc,
                                    mtcc_id)) {
                                flush_idcache_range((uintptr_t)address,
                                                    (uintptr_t)address, 4);
                            }
#endif
                            record->direct_call_slot = address;
                            record->direct_call_closed = *address;
                            record->direct_continuation = address + 1;
                            record->direct_continuation_invalid = 0x50000000U |
                                ((cont_offset >> 16) & 0x3ff) |
                                (((uint64_t)cont_offset << 10) & 0x3fffc00);
                            record->direct_call_linked = true;
                        } else {
#ifdef CONFIG_SOFTMMU
                            uint32_t *resolver = (void *)((uintptr_t)gen_code_buf +
                                ir2_size + (size_t)native_core_count * AOT_CORE_COLD_BYTES +
                                (size_t)native_direct_index++ * AOT_DIRECT_COLD_BYTES);
                            uint64_t source_id = tu_pages->id;
                            uint32_t cold_offset = rel->extra_addend;
                            if (ir2_size < 32 || cold_offset > ir2_size - 32) {
                                return NULL;
                            }
                            uint32_t *cold_tail = (void *)((uintptr_t)gen_code_buf +
                                                           cold_offset + 28);
                            uint32_t guard_branch = aot_native_guard_resolver(
                                resolver + AOT_DIRECT_RESOLVER_BYTES / sizeof(uint32_t),
                                address, record->tb, record->callee_pc, cold_tail, mtcc_id);
                            if (env->tu_generation_array && source_id &&
                                source_id < option_tucp_generation_array_size &&
                                source_id <= INT32_MAX && guard_branch &&
                                aot_native_call_resolver(resolver, address, record->tb,
                                                         record->callee_pc, mtcc_id,
                                                         false)) {
                                /* Before the first link, only the source exists.
                                 * Its generation protects the pending resolver;
                                 * a successful link replaces this with the target id. */
                                uint32_t *guard = address - 11;
                                guard[0] = 0x14000005U |
                                    (((source_id >> 12) & 0xfffff) << 5);
                                guard[1] = 0x038000a5U |
                                    ((source_id & 0xfff) << 10);
                                flush_idcache_range((uintptr_t)guard, (uintptr_t)guard, 8);
                                flush_idcache_range((uintptr_t)address, (uintptr_t)address, 4);
                                *cold_tail = guard_branch;
                                flush_idcache_range((uintptr_t)cold_tail,
                                                    (uintptr_t)cold_tail, 4);
                            }
#endif
                            record->direct_call_slot = address;
                            record->direct_call_closed = *address;
                            record->direct_continuation = address + 1;
                            record->direct_continuation_invalid = 0x50000000U |
                                ((cont_offset >> 16) & 0x3ff) |
                                (((uint64_t)cont_offset << 10) & 0x3fffc00);
                            record->direct_call_guards_emitted = true;
                        }
                    }
                } else {
                    record->gate = address;
                    record->gate_insn = *address;
                    record->stub = (void *)((uintptr_t)gen_code_buf +
                                           (uint32_t)rel->extra_addend);
                    record->continuation = (void *)((uintptr_t)gen_code_buf +
                                                   (rel->extra_addend >> 32));
                }
            }
        }
        if (info->count != native_count) {
            return NULL;
        }
        /* A leaf has no CALL record to bind its softRAS miss slot. */
        for (int m = 0; m < tb_num_in_tu; m++) {
            aot_block *block = &file_tb_vector[m];
            if (block->rel_start < 0) continue;
            for (int j = block->rel_start; j <= block->rel_end; j++) {
                aot_rel *rel = &curr_rel_table[j];
                if (rel->kind != AOT_NATIVE_RET_MISS) continue;
                uint32_t *slot = (void *)((uintptr_t)gen_code_buf +
                                  block->tc.offset_in_tu + rel->tc_offset);
                uintptr_t fallback = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
                int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)slot;
                int64_t offset = delta >> 2;
                if (!option_soft_ras || !option_rastoptp || !fallback ||
                    (delta & 3) || offset != sextract64(offset, 0, 36)) {
                    return NULL;
                }
                slot[0] = 0x1e000004U |
                    ((((offset + (1 << 15)) >> 16) & 0xfffff) << 5);
                slot[1] = 0x4c000080U | ((offset & 0xffff) << 10);
                flush_idcache_range((uintptr_t)slot, (uintptr_t)slot, 8);
            }
        }
        uint32_t cold_index = 0;
        for (uint32_t i = 0; i < info->count; i++) {
            AOTNativeRecord *record = &info->records[i];
            const NativeRecoverHeader *recover = record->recover;
            uint32_t direct_offset = record->direct_native_entry ? 32 : 0;
            if (!record->core || record->core != record->entry ||
                !recover || recover->state_bytes ||
                !aot_native_entry_replaceable(record->core,
                                              recover->code_bytes,
                                              direct_offset)) continue;
            TranslationBlock *owner = record->tb;
            uint32_t *cold = (void *)((uintptr_t)gen_code_buf + ir2_size +
                                      (size_t)cold_index * AOT_CORE_COLD_BYTES);
            uintptr_t fallback = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
            int64_t delta = (int64_t)fallback - (int64_t)(uintptr_t)&cold[3];
            int64_t offset = delta >> 2;
            int64_t branch = (intptr_t)cold - (intptr_t)record->core;
            int64_t branch_offset = branch >> 2;
            int64_t direct_branch = record->direct_native_entry ?
                ((intptr_t)cold - (intptr_t)record->direct_native_entry) : 0;
            int64_t direct_branch_offset = direct_branch >> 2;
            uint64_t guest_pc = owner->pc - owner->cs_base;
            if (!fallback || !option_soft_ras || !option_rastoptp ||
                !(owner->flags & HF_CS64_MASK) || owner->cs_base ||
                (owner->flags & HF_CPL_MASK) != HF_CPL_MASK ||
                (tb_cflags(owner) & (CF_INVALID | CF_NO_ACTIVE | CF_PCREL |
                                     CF_SINGLE_STEP | CF_USE_ICOUNT)) ||
                sextract64(guest_pc, 0, 52) != (int64_t)guest_pc ||
                (delta & 3) || offset != sextract64(offset, 0, 36) ||
                (branch & 3) || branch_offset != sextract64(branch_offset, 0, 26) ||
                (direct_branch & 3) ||
                direct_branch_offset != sextract64(direct_branch_offset, 0, 26)) {
                continue;
            }
            cold[0] = 0x14000005U | (((guest_pc >> 12) & 0xfffff) << 5);
            cold[1] = 0x038000a5U | ((guest_pc & 0xfff) << 10);
            cold[2] = 0x16000005U | (((guest_pc >> 32) & 0xfffff) << 5);
            cold[3] = 0x1e000004U |
                ((((offset + (1 << 15)) >> 16) & 0xfffff) << 5);
            cold[4] = 0x4c000080U | ((offset & 0xffff) << 10);
            flush_idcache_range((uintptr_t)cold, (uintptr_t)cold, 20);
            cold_index++;
            record->core_invalid_insn = 0x50000000U |
                ((branch_offset >> 16) & 0x3ff) |
                (((uint64_t)branch_offset << 10) & 0x3fffc00);
            if (record->direct_native_entry) {
                record->direct_entry_invalid_insn = 0x50000000U |
                    ((direct_branch_offset >> 16) & 0x3ff) |
                    (((uint64_t)direct_branch_offset << 10) & 0x3fffc00);
            }
#ifdef CONFIG_SOFTMMU
            if (option_int_opt == IRQ_CHECK_ASYNC_NOTIFY &&
                qemu_tcg_mttcg_enabled() &&
                current_machine->smp.max_cpus == 1) {
                owner->s_data->aot_native_call_entry = record->core;
                owner->s_data->aot_direct_native_entry =
                    record->direct_native_entry;
            }
#endif
        }
#ifdef CONFIG_SOFTMMU
        size_t sentinel_off = offsetof(CPUX86State, ras_tp.sentinel.ptr);
        if (summary.native_osr_count &&
            option_int_opt == IRQ_CHECK_ASYNC_NOTIFY &&
            qemu_tcg_mttcg_enabled() &&
            current_machine->smp.max_cpus == 1 &&
            !(sentinel_off & 3) && sentinel_off <= 32764) {
            uintptr_t fallback = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
            uint32_t *osr_cold_base = (void *)((uintptr_t)gen_code_buf +
                ir2_size +
                (size_t)native_core_count * AOT_CORE_COLD_BYTES +
                (size_t)native_direct_count * AOT_DIRECT_COLD_BYTES +
                (size_t)summary.native_indirect_count *
                    AOT_INDIRECT_COLD_BYTES +
                (size_t)summary.native_linked_count * AOT_LINKED_COLD_BYTES);
            uint32_t osr_stub_index = 0;
            for (uint32_t i = 0; i < info->count; i++) {
                AOTNativeRecord *record = &info->records[i];
                const NativeRecoverHeader *recover = record->recover;
                if (!record->core_invalid_insn ||
                    !record->direct_native_entry || !recover ||
                    recover->state_bytes || recover->points ||
                    !recover->osr_count ||
                    record->tb->s_data->aot_direct_native_entry !=
                        record->direct_native_entry) {
                    continue;
                }
                TranslationBlock *owner = record->tb;
                for (uint32_t k = 0;
                     k < recover->osr_count && k < NATIVE_RECOVER_MAX_OSR;
                     k++) {
                    if (osr_stub_index >= summary.native_osr_count) {
                        break;
                    }
                    int32_t guest_off = recover->osr_guest_offsets[k];
                    if (!guest_off) {
                        continue;
                    }
                    vaddr target_pc = owner->pc + (int64_t)guest_off;
                    TranslationBlock *osr_tb = NULL;
                    for (int m = 0; m < tb_num_in_tu; m++) {
                        TranslationBlock *cand = curr_tb_vector[m];
                        if (cand == owner || cand->pc != target_pc ||
                            cand->cs_base != owner->cs_base ||
                            cand->flags != owner->flags ||
                            tb_cflags(cand) != tb_cflags(owner) ||
                            (cand->misc_flags & AOT_BLOCK_OBSERVED_CALL_ENTRY) ||
                            cand->s_data->aot_native_call_entry ||
                            cand->s_data->aot_direct_native_entry) {
                            continue;
                        }
                        osr_tb = cand;
                        break;
                    }
                    if (!osr_tb) {
                        continue;
                    }
                    uint32_t *stub = osr_cold_base + (size_t)osr_stub_index * 10;
                    int64_t entry_delta = (intptr_t)record->direct_native_entry -
                                          (intptr_t)&stub[3];
                    int64_t entry_off = entry_delta >> 2;
                    int64_t fb_delta = (int64_t)fallback -
                                       (int64_t)(uintptr_t)&stub[7];
                    int64_t fb_off = fb_delta >> 2;
                    uint64_t osr_pc = osr_tb->pc - osr_tb->cs_base;
                    if (!fallback ||
                        (tb->flags & HF_CPL_MASK) != HF_CPL_MASK ||
                        (int64_t)osr_pc < 0 ||
                        sextract64(osr_pc, 0, 52) != (int64_t)osr_pc ||
                        (entry_delta & 3) ||
                        entry_off != sextract64(entry_off, 0, 26) ||
                        (fb_delta & 3) ||
                        fb_off != sextract64(fb_off, 0, 36)) {
                        continue;
                    }
                    stub[0] = 0x03800004U | ((k & 0xfffU) << 10);
                    stub[1] = 0x033fc084U;
                    stub[2] = 0x260002e1U |
                        ((((uint32_t)(sentinel_off >> 2)) & 0x3fffU) << 10);
                    stub[3] = 0x50000000U |
                        (((uint32_t)(entry_off >> 16)) & 0x3ffU) |
                        ((((uint32_t)entry_off) << 10) & 0x3fffc00U);
                    stub[4] = 0x14000005U | (((osr_pc >> 12) & 0xfffffU) << 5);
                    stub[5] = 0x038000a5U | ((osr_pc & 0xfffU) << 10);
                    stub[6] = 0x16000005U | (((osr_pc >> 32) & 0xfffffU) << 5);
                    stub[7] = 0x1e000004U |
                        ((((fb_off + (1 << 15)) >> 16) & 0xfffffU) << 5);
                    stub[8] = 0x4c000080U | ((fb_off & 0xffffU) << 10);
                    stub[9] = 0x03400000U;
                    flush_idcache_range((uintptr_t)stub, (uintptr_t)stub,
                                        AOT_OSR_STUB_BYTES);
                    osr_stub_index++;
                    uint32_t slot_idx = record->osr_count++;
                    record->osr_tbs[slot_idx] = osr_tb;
                    record->osr_entries[slot_idx] = stub;
                    osr_tb->s_data->aot_direct_native_entry = stub;
                }
            }
        }
#endif
        /* Member addresses, immutable fallback slots and all records are final
         * before any TB in this TU becomes visible to normal linking. */
        qatomic_store_release(&tu_pages->native, info);
    }

    if (option_pushpop) {
        for (int i = 0; i < tu_head->tb_count; i++) {
            tb = curr_tb_vector[i];
            if (!region_alloc_push_pop_data(tb, mtcc_id, tb->icount)) {
                aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
            }
            memcpy(tb_opt_ptr(tb), (void *)(search_code_pos + pre_search_size),
                    tb->icount << 2);
            pre_search_size += tb->icount << 2;
        }
    }

    for (int i = 0; i < tu_head->tb_count; i++) {
        tb = curr_tb_vector[i];
        if (option_soft_ras && tb->inst_ptr_offset) {
            if (option_rastoptp) {
                /* All member addresses are final, none published yet. */
                if (option_tu_hras == 2 && latxs_hras_paired_return(tb)) {
                    latxs_ras_tp_link_initial(cpu, tb, tb->s_data->next_tb[1]);
                    continue;
                }
                uint64_t return_pc = tb->pc + tb->size;
                for (int j = 0; j < tu_head->tb_count; ++j) {
                    TranslationBlock *next = curr_tb_vector[j];
                    if (next->pc == return_pc &&
                        (tb->pc & TARGET_PAGE_MASK) ==
                        (return_pc & TARGET_PAGE_MASK)) {
                        latxs_ras_tp_link_initial(cpu, tb, next);
                        break;
                    }
                }
                continue;
            }
            if (i + 1 == tu_head->tb_count) {
                continue;
            }
            TranslationBlock *next_tb = curr_tb_vector[i + 1];
            if (tb->s_data->x86_next_pc == next_tb->pc) {
                if (!latxs_ras_link(tb, next_tb)) {
                    latxs_ras_unlink(tb);
                }
            } else {
                /* latxs_ras_unlink(tb); */
            }
        }
    }

    if (existing_tu && native_core) {
        for (int i = 0; i < tb_num_in_tu; i++) {
            TranslationBlock *next = curr_tb_vector[i];
            TranslationBlock *current = tb_htable_lookup_noexit(cpu, next->pc,
                next->cs_base, next->flags, tb_cflags(next));
            if (current != conflicts[i] || (current &&
                (!aot_plain_conflict(cpu, current) ||
                 !aot_conflict_pages_locked(current)))) {
                return tb_htable_lookup_noexit(cpu, pc, cs_base, flags,
                                               cflags);
            }
        }
    }
    /* All code/records/RAS edges are complete. Register native source owners
     * and every host-PC record before exposing any member. Dependencies and
     * old-owner pages stay locked through per-member canonical publication. */
#ifndef CONFIG_USER_ONLY
    if (native_core && !tb_register_native_source_pages(curr_tb_vector[0])) {
        aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
    }
#endif
    for (int i = 0; i < tu_head->tb_count; i++) {
        tcg_tb_insert(curr_tb_vector[i]);
    }
    for (int i = 0; i < tu_head->tb_count; i++) {
        tb = curr_tb_vector[i];
#ifdef CONFIG_LATX_PRINT
        if (tb->flags & HF_CPL_MASK) {
            tb_aot_cnt[cpu->cpu_index + 4]++;
        } else {
            tb_aot_cnt[cpu->cpu_index]++;
        }
#endif
        TranslationBlock *linked = native_core ?
            aot_tb_replace_page(tb, conflicts[i]) :
            aot_tb_link_page(tb, owner_local_returns);
        loaded_count += linked == tb;
#if DEBUG_AOT
        load_user_tb_count++;
#endif
        if (tb->pc == pc &&
                tb->cflags == cflags &&
                tb->flags == flags) {
            ret = linked;
        }
    }
    if (native_core) {
        /* Partial canonical/private ownership is valid, like HRAS members.
         * Return the real key owner, never an unregistered vector member. */
        ret = tb_htable_lookup_noexit(cpu, pc, cs_base, flags, cflags);
    }

    check_tu(cpu, tu_head, curr_tb_vector, file_tb_vector, tbmini_ptr, ir2_size);
    if (loaded_tb_count) {
        *loaded_tb_count = loaded_count;
    }
    return ret;
}

static bool aot_tu_matches(const aot_tu_head *tu_head,
        vaddr anchor_page, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags)
{
    aot_block *blocks = aot_tu_blocks((aot_tu_head *)tu_head);

    for (uint32_t i = 0; i < tu_head->tb_count; i++) {
        if (aot_tu_rebase_pc(tu_head, anchor_page, blocks[i].pc) == pc &&
                blocks[i].cs_base == cs_base &&
                blocks[i].flags == flags &&
                blocks[i].cflags == cflags) {
            return true;
        }
    }
    return false;
}

static bool aot_tu_has_page(const aot_tu_head *tu_head, int64_t pc_delta)
{
    const aot_tu_page *pages = aot_tu_pages((aot_tu_head *)tu_head);

    for (uint32_t i = 0; i < tu_head->page_count; i++) {
        if (pages[i].pc_delta == pc_delta) {
            return true;
        }
    }
    return false;
}

static TranslationBlock *aot_try_load_tu(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags, vaddr hva,
        uint64_t file_disp, vaddr anchor_page, ENV *lsenv,
        uint32_t *candidate_tb_count, uint32_t *loaded_tb_count)
{
    aot_tu_head *tu_head = (aot_tu_head *)file_disp;

    if ((tu_head->page_count > 1 && !aot_cross_page_tu_enabled()) ||
            !aot_tu_matches(tu_head, anchor_page, pc, cs_base, flags,
                            cflags)) {
        return NULL;
    }
    if (aot_tu_still_blocked(cpu, tu_head, anchor_page, cs_base)) {
        return NULL;
    }
    *candidate_tb_count += tu_head->tb_count;
    AOTTUStaticSummary summary = aot_tu_static_summary(tu_head, anchor_page);
    bool native_core = summary.native_core_count != 0;
    if (!native_core) {
        if (!aot_lock_tu_dependencies(cpu, anchor_page, tu_head, NULL, 0)) {
            return NULL;
        }
        TranslationBlock *tb = load_tu(cpu, pc, cs_base, flags, cflags, hva,
                file_disp, anchor_page, lsenv, loaded_tb_count);
        aot_unlock_load_pages(-1);
        return tb;
    }
    g_assert(aot_tu_conflicts == NULL &&
             aot_tu_conflict_source_pages == NULL);
    GArray *extra = g_array_new(false, false, sizeof(tb_page_addr_t));
    aot_tu_conflict_source_pages = extra;
    TranslationBlock *tb = NULL;
    aot_block *blocks = aot_tu_blocks(tu_head);
    /* A conflict's old TU can depend on pages absent from the new candidate.
     * Drop all locks, sort the union and revalidate mappings/bytes each time;
     * never acquire a lower page while holding a higher one. */
    for (unsigned retry = 0; retry < 8; retry++) {
        if (!aot_lock_tu_dependencies(cpu, anchor_page, tu_head,
                (const tb_page_addr_t *)extra->data, extra->len)) {
            break;
        }
        unsigned before = extra->len;
        for (unsigned i = 0; i < tu_head->tb_count; i++) {
            vaddr member_pc = aot_tu_rebase_pc(tu_head, anchor_page,
                                               blocks[i].pc);
            TranslationBlock *old = tb_htable_lookup_noexit(cpu, member_pc,
                cs_base, blocks[i].flags, blocks[i].cflags);
            if (aot_plain_conflict(cpu, old)) {
                aot_conflict_pages(extra, old);
            }
        }
        if (before == extra->len) {
            tb = load_tu(cpu, pc, cs_base, flags, cflags, hva,
                    file_disp, anchor_page, lsenv, loaded_tb_count);
            aot_unlock_load_pages(-1);
            break;
        }
        aot_unlock_load_pages(-1);
    }
    aot_clear_tu_conflicts();
    return tb;
}

static TranslationBlock *aot_load_tu_ref(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags,
        const aot_tu_ref *ref, vaddr container_page, ENV *lsenv,
        uint32_t *candidate_tb_count, uint32_t *loaded_tb_count)
{
    vaddr anchor_page = container_page + ref->anchor_delta;
    ADDR anchor_hva = 0;
    uint64_t anchor_file = aot_find_tu_page_record(cpu, anchor_page,
            &anchor_hva, NULL);

    if (!anchor_file) {
        return NULL;
    }

    page_head *anchor_head = (page_head *)anchor_file;
    uint64_t record = anchor_file + sizeof(*anchor_head) + TARGET_PAGE_SIZE;
    for (uint32_t i = 0; i < anchor_head->tb_count; i++) {
        uint32_t magic = *(uint32_t *)record;

        if (magic == AOT_TB_MAGIC) {
            aot_tu_head *tu_head = (aot_tu_head *)record;
            if (tu_head->tu_id == ref->tu_id &&
                    aot_tu_has_page(tu_head, -ref->anchor_delta)) {
                TranslationBlock *tb = aot_try_load_tu(cpu, pc, cs_base,
                        flags, cflags, anchor_hva, record, anchor_page, lsenv,
                        candidate_tb_count, loaded_tb_count);
                if (tb) {
                    return tb;
                }
            }
            record += tu_head->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            record += sizeof(aot_tu_ref);
        } else {
            return NULL;
        }
    }
    return NULL;
}

TranslationBlock *load_page_tu(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, vaddr hva, uint64_t file_disp, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    bool perf_enabled = aot_perf_enabled();
    uint64_t perf_begin = perf_enabled ? aot_perf_now_ns() : 0;
    TranslationBlock *ret_tb = NULL;
    page_head *ph = (page_head *)file_disp;
    TranslationBlock *curr_tb;
    uint32_t record_count = 0;
    uint32_t candidate_tb_count = 0;
    uint32_t registered_activated_count = 0;
    vaddr container_page = pc & TARGET_PAGE_MASK;
    file_disp += sizeof(page_head) + TARGET_PAGE_SIZE;

    while (record_count < ph->tb_count) {
#if DEBUG_AOT
        uint64_t gen_code_begin = get_tsc();
#endif
        uint32_t loaded_tb_count = 0;
        uint32_t magic = *(uint32_t *)file_disp;
        curr_tb = NULL;
        if (magic == AOT_TB_MAGIC) {
            aot_tu_head *tu_head = (aot_tu_head *)file_disp;

            curr_tb = aot_try_load_tu(cpu, pc, cs_base, flags, cflags,
                    hva, file_disp, container_page, lsenv,
                    &candidate_tb_count, &loaded_tb_count);
            file_disp += tu_head->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            aot_tu_ref *ref = (aot_tu_ref *)file_disp;

            if (aot_cross_page_tu_enabled()) {
                curr_tb = aot_load_tu_ref(cpu, pc, cs_base, flags, cflags,
                        ref, container_page, lsenv, &candidate_tb_count,
                        &loaded_tb_count);
            }
            file_disp += sizeof(*ref);
        } else {
            break;
        }
        registered_activated_count += loaded_tb_count;
#if DEBUG_AOT
        load_tu_time += get_tsc() - gen_code_begin;
#endif
        if ((uintptr_t)curr_tb == -1) {
            break;
        }
        if (curr_tb && curr_tb->pc == pc
                && curr_tb->flags == flags
                && curr_tb->cflags == cflags) {
            ret_tb = curr_tb;
            break;
        }
        record_count++;
    }

#if DEBUG_AOT
    load_page_tu_time += get_tsc() - begin;
#endif
    if (perf_enabled) {
        aot_perf_record_register_activate_tb(AOT_USER, candidate_tb_count,
                registered_activated_count, aot_perf_now_ns() - perf_begin);
    }
    return ret_tb;
}

TranslationBlock *load_aot_tu(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    ADDR hva = 0;
    uint64_t file_addr = aot_find_tu_page_record(cpu,
            pc & TARGET_PAGE_MASK, &hva, NULL);

    if (!file_addr) {
        return NULL;
    }
    TranslationBlock *tb = load_page_tu(cpu, pc, cs_base, flags, cflags,
                                        hva, file_addr, lsenv);
#if DEBUG_AOT
    load_aot_time += (get_tsc() - begin);
#endif

    return tb;
}
#endif

static bool aot_load_select_index(uint32_t flags,
        AOTLoadIndexSelection *selection)
{
    AOTPublishedFile *file;

    if (flags & HF_CPL_MASK) {
        file = qatomic_rcu_read(&user_aot_file);
        *selection = (AOTLoadIndexSelection) {
            .file = file,
            .tb_ptr = file ? file->tb_ptr : NULL,
            .hash_table = file ? file->hash_table : NULL,
            .hash_size = file ? file->hash_size : 0,
            .file_generation = file ? file->generation : 0,
            .aot_mode = AOT_USER,
#ifdef CONFIG_LATX_TU
            .use_tu = aot_tu_enabled(),
#else
            .use_tu = false,
#endif
        };
    } else {
        file = qatomic_rcu_read(&core_aot_file);
        *selection = (AOTLoadIndexSelection) {
            .file = file,
            .tb_ptr = file ? file->tb_ptr : NULL,
            .hash_table = file ? file->hash_table : NULL,
            .hash_size = file ? file->hash_size : 0,
            .file_generation = file ? file->generation : 0,
            .aot_mode = AOT_CORE,
            .use_tu = false,
        };
    }

    return selection->tb_ptr != NULL;
}

static void aot_load_bind_index(const AOTLoadIndexSelection *selection)
{
    in_tb_ptr = selection->tb_ptr;
    hash_table = selection->hash_table;
    aot_hash_size = selection->hash_size;
    aot_file_generation = selection->file_generation;
}

static TranslationBlock *aot_load_dispatch_selected(
        const AOTLoadIndexSelection *selection, CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags, ENV *lsenv)
{
#ifdef CONFIG_LATX_TU
    if (selection->use_tu) {
        return load_aot_tu(cpu, pc, cs_base, flags, cflags, lsenv);
    }
#else
    (void)selection;
#endif
    return load_aot_tb(cpu, pc, cs_base, flags, cflags, lsenv);
}

TranslationBlock *load_aot(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv)
{
    AOTLoadIndexSelection selection;

    if(!option_sys_aot || !option_aot_load) {
        return NULL;
    }
#ifdef CONFIG_SOFTMMU
    if (hamt_enabled() && !can_do_hamt(lsenv->cpu_state)) {
        return NULL;
    }
#endif

    if (!aot_load_select_index(flags, &selection)) {
        return NULL;
    }
    aot_load_bind_index(&selection);
    return aot_load_dispatch_selected(&selection, cpu, pc, cs_base, flags,
            cflags, lsenv);
}
