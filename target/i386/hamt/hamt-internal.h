/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_INTERNAL_H_
#define _HAMT_INTERNAL_H_

#include "hamt-cpu.h"
#include "qemu/bitops.h"
#include "qemu/typedefs.h"

typedef enum {
    OPC_LD_B     = 0b0010100000 << 22,
    OPC_LD_H     = 0b0010100001 << 22,
    OPC_LD_W     = 0b0010100010 << 22,
    OPC_LD_D     = 0b0010100011 << 22,
    OPC_ST_B     = 0b0010100100 << 22,
    OPC_ST_H     = 0b0010100101 << 22,
    OPC_ST_W     = 0b0010100110 << 22,
    OPC_ST_D     = 0b0010100111 << 22,
    OPC_LD_BU    = 0b0010101000 << 22,
    OPC_LD_HU    = 0b0010101001 << 22,
    OPC_LD_WU    = 0b0010101010 << 22,

    OPC_LDX_B    = 0b00111000000000 << 18,
    OPC_LDX_H    = 0b00111000000001 << 18,
    OPC_LDX_W    = 0b00111000000010 << 18,
    OPC_LDX_D    = 0b00111000000011 << 18,
    OPC_STX_B    = 0b00111000000100 << 18,
    OPC_STX_H    = 0b00111000000101 << 18,
    OPC_STX_W    = 0b00111000000110 << 18,
    OPC_STX_D    = 0b00111000000111 << 18,
    OPC_LDX_BU   = 0b00111000001000 << 18,
    OPC_LDX_HU   = 0b00111000001001 << 18,
    OPC_LDX_WU   = 0b00111000001010 << 18,

    OPC_LDPTR_W  = 0b00100100 << 24,
    OPC_STPTR_W  = 0b00100101 << 24,
    OPC_LDPTR_D  = 0b00100110 << 24,
    OPC_STPTR_D  = 0b00100111 << 24,

    OPC_FLD_S    = 0x2B000000,
    OPC_FST_S    = 0x2B400000,
    OPC_FLD_D    = 0x2B800000,
    OPC_FST_D    = 0x2BC00000,
    OPC_VLD      = 0x2C000000,
    OPC_VST      = 0x2C400000,
    OPC_XVLD     = 0x2C800000,
    OPC_XVST     = 0x2CC00000,
    OPC_VLDX     = 0x38400000,
    OPC_VSTX     = 0x38440000,
    OPC_XVLDX    = 0x38480000,
    OPC_XVSTX    = 0x384C0000,
    OPC_VSTELM_D = 0x31000000, //This opc covers VSTELM_D and VSTELM_W

    OPC_LL_W        = 0b00100000 << 24,
    OPC_SC_W        = 0b00100001 << 24,
    OPC_LL_D        = 0b00100010 << 24,
    OPC_SC_D        = 0b00100011 << 24,

    OPC_SC_Q        = 0b00111000010101110 << 15,
    OPC_AMCAS_B     = 0b00111000010110000 << 15,
    OPC_AMCAS_H     = 0b00111000010110001 << 15,
    OPC_AMCAS_W     = 0b00111000010110010 << 15,
    OPC_AMCAS_D     = 0b00111000010110011 << 15,
    OPC_AMCAS_DB_B  = 0b00111000010110100 << 15,
    OPC_AMCAS_DB_H  = 0b00111000010110101 << 15,
    OPC_AMCAS_DB_W  = 0b00111000010110110 << 15,
    OPC_AMCAS_DB_D  = 0b00111000010110111 << 15,
    OPC_AMSWAP_B    = 0b00111000010111000 << 15,
    OPC_AMSWAP_H    = 0b00111000010111001 << 15,
    OPC_AMADD_B     = 0b00111000010111010 << 15,
    OPC_AMADD_H     = 0b00111000010111011 << 15,
    OPC_AMSWAP_DB_B = 0b00111000010111100 << 15,
    OPC_AMSWAP_DB_H = 0b00111000010111101 << 15,
    OPC_AMADD_DB_B  = 0b00111000010111110 << 15,
    OPC_AMADD_DB_H  = 0b00111000010111111 << 15,
    OPC_AMSWAP_W    = 0b00111000011000000 << 15,
    OPC_AMSWAP_D    = 0b00111000011000001 << 15,
    OPC_AMADD_W     = 0b00111000011000010 << 15,
    OPC_AMADD_D     = 0b00111000011000011 << 15,
    OPC_AMAND_W     = 0b00111000011000100 << 15,
    OPC_AMAND_D     = 0b00111000011000101 << 15,
    OPC_AMOR_W      = 0b00111000011000110 << 15,
    OPC_AMOR_D      = 0b00111000011000111 << 15,
    OPC_AMXOR_W     = 0b00111000011001000 << 15,
    OPC_AMXOR_D     = 0b00111000011001001 << 15,
    OPC_AMMAX_W     = 0b00111000011001010 << 15,
    OPC_AMMAX_D     = 0b00111000011001011 << 15,
    OPC_AMMIN_W     = 0b00111000011001100 << 15,
    OPC_AMMIN_D     = 0b00111000011001101 << 15,
    OPC_AMMAX_WU    = 0b00111000011001110 << 15,
    OPC_AMMAX_DU    = 0b00111000011001111 << 15,
    OPC_AMMIN_WU    = 0b00111000011010000 << 15,
    OPC_AMMIN_DU    = 0b00111000011010001 << 15,
    OPC_AMSWAP_DB_W = 0b00111000011010010 << 15,
    OPC_AMSWAP_DB_D = 0b00111000011010011 << 15,
    OPC_AMADD_DB_W  = 0b00111000011010100 << 15,
    OPC_AMADD_DB_D  = 0b00111000011010101 << 15,
    OPC_AMAND_DB_W  = 0b00111000011010110 << 15,
    OPC_AMAND_DB_D  = 0b00111000011010111 << 15,
    OPC_AMOR_DB_W   = 0b00111000011011000 << 15,
    OPC_AMOR_DB_D   = 0b00111000011011001 << 15,
    OPC_AMXOR_DB_W  = 0b00111000011011010 << 15,
    OPC_AMXOR_DB_D  = 0b00111000011011011 << 15,
    OPC_AMMAX_DB_W  = 0b00111000011011100 << 15,
    OPC_AMMAX_DB_D  = 0b00111000011011101 << 15,
    OPC_AMMIN_DB_W  = 0b00111000011011110 << 15,
    OPC_AMMIN_DB_D  = 0b00111000011011111 << 15,
    OPC_AMMAX_DB_WU = 0b00111000011100000 << 15,
    OPC_AMMAX_DB_DU = 0b00111000011100001 << 15,
    OPC_AMMIN_DB_WU = 0b00111000011100010 << 15,
    OPC_AMMIN_DB_DU = 0b00111000011100011 << 15,
} ld_st_inst;

/* Every HAMT MMIO class uses the ordinary signed 12-bit displacement. */
static inline int hamt_simm12(uint32_t inst)
{
    return sextract32(inst, 10, 12);
}

/* 0: unsupported, 1: scalar base, 2: scalar indexed, 3: PTR,
 * 4: vector base, 5: vector indexed. */
int hamt_mmio_inst_type(uint32_t inst);

/* PTR memory instructions use an eight-bit opcode and scaled si14. */
static inline bool hamt_is_ptr_mem(uint32_t inst)
{
    return (inst & 0xfc000000u) == OPC_LDPTR_W;
}

static inline bool hamt_is_vector_mem(uint32_t inst)
{
    uint32_t opc = inst & 0xffc00000u;
    return opc == OPC_VLD || opc == OPC_VST ||
           opc == OPC_XVLD || opc == OPC_XVST;
}

static inline bool hamt_is_vector_indexed_mem(uint32_t inst)
{
    return (inst & 0xffff8000u) == OPC_VLDX ||
           (inst & 0xffff8000u) == OPC_VSTX ||
           (inst & 0xffff8000u) == OPC_XVLDX ||
           (inst & 0xffff8000u) == OPC_XVSTX;
}

static inline int hamt_ptr_offset(uint32_t inst)
{
    int imm = (inst >> 10) & 0x3fff;
    return ((imm ^ 0x2000) - 0x2000) * 4;
}

static inline uint32_t hamt_indexed_mem_opcode(uint32_t inst)
{
    /* The three register fields occupy bits 14:0. Keep bits 17:15 in
     * the match: other 0x38 instructions share the coarse enum prefix. */
    switch (inst & 0xffff8000u) {
    case OPC_LDX_B: return OPC_LD_B;
    case OPC_LDX_H: return OPC_LD_H;
    case OPC_LDX_W: return OPC_LD_W;
    case OPC_LDX_D: return OPC_LD_D;
    case OPC_LDX_BU: return OPC_LD_BU;
    case OPC_LDX_HU: return OPC_LD_HU;
    case OPC_LDX_WU: return OPC_LD_WU;
    case OPC_STX_B: return OPC_ST_B;
    case OPC_STX_H: return OPC_ST_H;
    case OPC_STX_W: return OPC_ST_W;
    case OPC_STX_D: return OPC_ST_D;
    case OPC_VLDX: return OPC_VLD;
    case OPC_VSTX: return OPC_VST;
    case OPC_XVLDX: return OPC_XVLD;
    case OPC_XVSTX: return OPC_XVST;
    default: return 0;
    }
}

static inline uint32_t hamt_mem_opcode(uint32_t inst)
{
    uint32_t indexed = hamt_indexed_mem_opcode(inst);
    if (indexed) {
        return indexed;
    }
    switch (inst & 0xff000000u) {
    case OPC_LDPTR_W: return OPC_LD_W;
    case OPC_STPTR_W: return OPC_ST_W;
    case OPC_LDPTR_D: return OPC_LD_D;
    case OPC_STPTR_D: return OPC_ST_D;
    default: return inst & 0xffc00000u;
    }
}


#include "loongarch-csr.h"

#define PS_MASK     0x3f000000
#define PS_SHIFT    24
#define PS_4K       0x0000000c
#define PS_8K       0x0000000d
#define PS_16K      0x0000000e
#define PS_32K      0x0000000f
#define PS_64K      0x00000010
#define PS_128K     0x00000011
#define PS_256K     0x00000012
#define PS_512K     0x00000013
#define PS_1M       0x00000014
#define PS_2M       0x00000015
#define PS_4M       0x00000016
#define PS_8M       0x00000017
#define PS_16M      0x00000018
#define PS_32M      0x00000019
#define PS_64M      0x0000001a
#define PS_256M     0x0000001b
#define PS_1G       0x0000001c

void loongarch_irq_enable(void);
void loongarch_ertn(void);
void restore_prmd(int cpuid);
void cpu_loop_exit_recover_ptn_eflag(CPUState *cpu, uintptr_t epc);

#define DEF_CSR_READ(csr, num)                 \
static inline uint64_t read_csr_##csr(void)    \
{                                              \
    uint64_t val = 0;                          \
    asm volatile (                             \
        "csrrd %[v], %[reg] \n\t"              \
        : [v] "=r" (val)                       \
        : [reg] "i" (num)                      \
        : "memory");                           \
    return val;                                \
}

#define DEF_CSR_WRITE(csr, num)                     \
static inline void write_csr_##csr(uint64_t val)    \
{                                                   \
    asm volatile (                                  \
        "csrwr %[v], %[reg] \n\t"                   \
        : [v] "+r" (val)                            \
        : [reg] "i" (num)                           \
        : "memory");                                \
}

DEF_CSR_READ(crmd, LOONGARCH_CSR_CRMD)
DEF_CSR_READ(ecfg, LOONGARCH_CSR_ECFG)
DEF_CSR_WRITE(ecfg, LOONGARCH_CSR_ECFG)
DEF_CSR_READ(prmd, LOONGARCH_CSR_PRMD)
DEF_CSR_WRITE(prmd, LOONGARCH_CSR_PRMD)

DEF_CSR_READ(estat, LOONGARCH_CSR_ESTAT)
DEF_CSR_WRITE(estat, LOONGARCH_CSR_ESTAT)

DEF_CSR_READ(asid, LOONGARCH_CSR_ASID)
DEF_CSR_WRITE(asid, LOONGARCH_CSR_ASID)
DEF_CSR_READ(tlbehi, LOONGARCH_CSR_TLBEHI)
DEF_CSR_WRITE(tlbehi, LOONGARCH_CSR_TLBEHI)
DEF_CSR_READ(tlbidx, LOONGARCH_CSR_TLBIDX)
DEF_CSR_WRITE(tlbidx, LOONGARCH_CSR_TLBIDX)
DEF_CSR_READ(tlbelo0, LOONGARCH_CSR_TLBELO0)
DEF_CSR_WRITE(tlbelo0, LOONGARCH_CSR_TLBELO0)
DEF_CSR_READ(tlbelo1, LOONGARCH_CSR_TLBELO1)
DEF_CSR_WRITE(tlbelo1, LOONGARCH_CSR_TLBELO1)

DEF_CSR_READ(tlbr_ehi, LOONGARCH_CSR_TLBREHI)
DEF_CSR_WRITE(tlbr_ehi, LOONGARCH_CSR_TLBREHI)
DEF_CSR_READ(tlbr_elo0, LOONGARCH_CSR_TLBRELO0)
DEF_CSR_WRITE(tlbr_elo0, LOONGARCH_CSR_TLBRELO0)
DEF_CSR_READ(tlbr_elo1, LOONGARCH_CSR_TLBRELO1)
DEF_CSR_WRITE(tlbr_elo1, LOONGARCH_CSR_TLBRELO1)

/* Timer registers */
DEF_CSR_READ(timerid, LOONGARCH_CSR_TIMERID)
DEF_CSR_READ(timercfg, LOONGARCH_CSR_TIMERCFG)
DEF_CSR_WRITE(timercfg, LOONGARCH_CSR_TIMERCFG)
DEF_CSR_READ(timertick, LOONGARCH_CSR_TIMERTICK)
DEF_CSR_WRITE(timertick, LOONGARCH_CSR_TIMERTICK)
DEF_CSR_READ(timeroffset, LOONGARCH_CSR_TIMEROFFSET)
DEF_CSR_WRITE(timeroffset, LOONGARCH_CSR_TIMEROFFSET)
DEF_CSR_READ(timerclr, LOONGARCH_CSR_TINTCLR)
DEF_CSR_WRITE(timerclr, LOONGARCH_CSR_TINTCLR)

#endif
