/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _IR1_H_
#define _IR1_H_

#include "../capStone/include/capstone/capstone.h"
#include "common.h"

extern __thread csh handle;
extern __thread csh handle16;
extern __thread csh handle64;

#define eax_index 0
#define ecx_index 1
#define edx_index 2
#define ebx_index 3
#define esp_index 4
#define ebp_index 5
#define esi_index 6
#define edi_index 7
#define rax_index eax_index
#define rcx_index ecx_index
#define rdx_index edx_index
#define rbx_index ebx_index
#define rsp_index esp_index
#define rbp_index ebp_index
#define rsi_index esi_index
#define rdi_index edi_index
#define r8_index  8
#define r9_index  9
#define r10_index 10
#define r11_index 11
#define r12_index 12
#define r13_index 13
#define r14_index 14
#define r15_index 15

#define es_index 0
#define cs_index 1
#define ss_index 2
#define ds_index 3
#define fs_index 4
#define gs_index 5

#define CF_USEDEF_BIT_INDEX 0
#define PF_USEDEF_BIT_INDEX 1
#define AF_USEDEF_BIT_INDEX 2
#define ZF_USEDEF_BIT_INDEX 3
#define SF_USEDEF_BIT_INDEX 4
#define OF_USEDEF_BIT_INDEX 5
#define DF_USEDEF_BIT_INDEX 6

#define CF_USEDEF_BIT (1 << CF_USEDEF_BIT_INDEX)
#define PF_USEDEF_BIT (1 << PF_USEDEF_BIT_INDEX)
#define AF_USEDEF_BIT (1 << AF_USEDEF_BIT_INDEX)
#define ZF_USEDEF_BIT (1 << ZF_USEDEF_BIT_INDEX)
#define SF_USEDEF_BIT (1 << SF_USEDEF_BIT_INDEX)
#define OF_USEDEF_BIT (1 << OF_USEDEF_BIT_INDEX)
#define DF_USEDEF_BIT (1 << DF_USEDEF_BIT_INDEX)

typedef x86_insn IR1_OPCODE;
typedef x86_op_type IR1_OPND_TYPE;
typedef cs_x86_op IR1_OPND;
typedef x86_prefix IR1_PREFIX;
typedef x86_insn_group IR1_OPCODE_TYPE;

//#define MAX_IR1_NUM_PER_TB 255
#define MAX_IR1_NUM_PER_TB 128

typedef struct IR1_INST {
    cs_insn *info;
    uint8_t _eflag_use;
    uint8_t _eflag_def;
    uint8_t _eflag_opt;
    uint8_t flags;
    uint8_t shbr_flags;
    uint16_t ghbr_opt;
    uint16_t shbr_restore_zero64_before;
    uint16_t shbr_restore_zero64_after;
    uint16_t shbr_restore_zero64_exit[2];
    struct {
        uint64_t opc;
        int next; /* index of IR1 list */
    } instptn;
} IR1_INST;

#define IR1_EFOPT_INIT  0
#define IR1_EFOPT_NONE  1

extern IR1_OPND al_ir1_opnd;
extern IR1_OPND ah_ir1_opnd;
extern IR1_OPND ax_ir1_opnd;
extern IR1_OPND eax_ir1_opnd;
extern IR1_OPND rax_ir1_opnd;

extern IR1_OPND bl_ir1_opnd;
extern IR1_OPND bh_ir1_opnd;
extern IR1_OPND bx_ir1_opnd;
extern IR1_OPND ebx_ir1_opnd;
extern IR1_OPND rbx_ir1_opnd;

extern IR1_OPND cl_ir1_opnd;
extern IR1_OPND ch_ir1_opnd;
extern IR1_OPND cx_ir1_opnd;
extern IR1_OPND ecx_ir1_opnd;
extern IR1_OPND rcx_ir1_opnd;

extern IR1_OPND dl_ir1_opnd;
extern IR1_OPND dh_ir1_opnd;
extern IR1_OPND dx_ir1_opnd;
extern IR1_OPND edx_ir1_opnd;
extern IR1_OPND rdx_ir1_opnd;

extern IR1_OPND sp_ir1_opnd;
extern IR1_OPND esp_ir1_opnd;
extern IR1_OPND rsp_ir1_opnd;

extern IR1_OPND bp_ir1_opnd;
extern IR1_OPND ebp_ir1_opnd;
extern IR1_OPND rbp_ir1_opnd;

extern IR1_OPND si_ir1_opnd;
extern IR1_OPND esi_ir1_opnd;
extern IR1_OPND rsi_ir1_opnd;

extern IR1_OPND di_ir1_opnd;
extern IR1_OPND edi_ir1_opnd;
extern IR1_OPND rdi_ir1_opnd;

extern IR1_OPND eax_mem8_ir1_opnd;
extern IR1_OPND ecx_mem8_ir1_opnd;
extern IR1_OPND edx_mem8_ir1_opnd;
extern IR1_OPND ebx_mem8_ir1_opnd;
extern IR1_OPND esp_mem8_ir1_opnd;
extern IR1_OPND ebp_mem8_ir1_opnd;
extern IR1_OPND esi_mem8_ir1_opnd;
extern IR1_OPND edi_mem8_ir1_opnd;

extern IR1_OPND eax_mem16_ir1_opnd;
extern IR1_OPND ecx_mem16_ir1_opnd;
extern IR1_OPND edx_mem16_ir1_opnd;
extern IR1_OPND ebx_mem16_ir1_opnd;
extern IR1_OPND esp_mem16_ir1_opnd;
extern IR1_OPND ebp_mem16_ir1_opnd;
extern IR1_OPND esi_mem16_ir1_opnd;
extern IR1_OPND edi_mem16_ir1_opnd;

extern IR1_OPND eax_mem32_ir1_opnd;
extern IR1_OPND ecx_mem32_ir1_opnd;
extern IR1_OPND edx_mem32_ir1_opnd;
extern IR1_OPND ebx_mem32_ir1_opnd;
extern IR1_OPND esp_mem32_ir1_opnd;
extern IR1_OPND ebp_mem32_ir1_opnd;
extern IR1_OPND esi_mem32_ir1_opnd;
extern IR1_OPND edi_mem32_ir1_opnd;

// TODO : avx_bcast
inline __attribute__ ((always_inline))
void ir1_opnd_build_reg(IR1_OPND *opnd, int size, x86_reg reg)
{
    opnd->type = X86_OP_REG;
    lsassert(size % 8 == 0);
    opnd->size = size / 8;
    opnd->reg = reg;
    opnd->avx_bcast = X86_AVX_BCAST_INVALID;
}
inline __attribute__ ((always_inline))
void ir1_opnd_build_imm(IR1_OPND *opnd, int size, int64_t imm)
{
    opnd->type = X86_OP_IMM;
    lsassert(size % 8 == 0);
    opnd->size = size / 8;
    opnd->imm = imm;
    opnd->avx_bcast = X86_AVX_BCAST_INVALID;
}
inline __attribute__ ((always_inline))
void ir1_opnd_build_mem(IR1_OPND *opnd, int size, x86_reg base, int64_t disp)
{
    opnd->type = X86_OP_MEM;
    lsassert(size % 8 == 0);
    opnd->size = size / 8;
    opnd->mem.base = base;
    opnd->mem.disp = disp;
    opnd->mem.index = X86_REG_INVALID;
    opnd->mem.scale = 0;
    opnd->mem.segment = X86_REG_INVALID;
    opnd->avx_bcast = X86_AVX_BCAST_INVALID;
}
inline __attribute__ ((always_inline))
void ir1_opnd_build_full_mem(IR1_OPND *opnd, int size,
        x86_reg seg, x86_reg base, int64_t disp,
        int64_t index, int64_t scale)
{
    opnd->type = X86_OP_MEM;
    lsassert(size % 8 == 0);
    opnd->size = size / 8;
    opnd->mem.segment = seg;
    opnd->mem.base    = base;
    opnd->mem.disp  = disp;
    opnd->mem.index = index;
    opnd->mem.scale = scale;
    opnd->avx_bcast = X86_AVX_BCAST_INVALID;
}
inline __attribute__ ((always_inline))
IR1_OPND_TYPE ir1_opnd_type(IR1_OPND *opnd) { return opnd->type; }
inline __attribute__ ((always_inline))
int ir1_opnd_size(IR1_OPND *opnd) { return opnd->size << 3; }
inline __attribute__ ((always_inline))
int ir1_opnd_base_reg_bits_start(IR1_OPND *opnd)
{
    lsassert(opnd->type == X86_OP_REG && opnd->reg > X86_REG_INVALID &&
             opnd->reg < X86_REG_ENDING);
    switch (opnd->reg) {
    case X86_REG_AH:
    case X86_REG_BH:
    case X86_REG_CH:
    case X86_REG_DH:
        return 8;
    default:
        return 0;
    }
}
inline __attribute__ ((always_inline))
x86_reg ir1_opnd_index_reg(IR1_OPND *opnd) { return opnd->mem.index; }
inline __attribute__ ((always_inline))
x86_reg ir1_opnd_base_reg(IR1_OPND *opnd) { return opnd->mem.base; }
inline __attribute__ ((always_inline))
int ir1_opnd_scale(IR1_OPND *opnd) { return opnd->mem.scale; }
longx ir1_opnd_simm(IR1_OPND *opnd);
inline __attribute__ ((always_inline))
ulongx ir1_opnd_uimm(IR1_OPND *opnd)
{
    if (ir1_opnd_type(opnd) == X86_OP_IMM) {
        return (ulongx)(opnd->imm);
    } else if (ir1_opnd_type(opnd) == X86_OP_MEM) {
        return (ulongx)(opnd->mem.disp);
    } else {
        lsassertm(0, "REG opnd has no imm\n");
    }
    abort();
}
inline __attribute__ ((always_inline))
x86_reg ir1_opnd_seg_reg(IR1_OPND *opnd) { return opnd->mem.segment; }
inline __attribute__ ((always_inline))
int ir1_opnd_is_imm(IR1_OPND *opnd) { return opnd->type == X86_OP_IMM; }
inline __attribute__ ((always_inline))
int ir1_opnd_is_8h(IR1_OPND *opnd)
{
    return opnd->type == X86_OP_REG && opnd->size == 1 &&
           (opnd->reg == X86_REG_AH || opnd->reg == X86_REG_BH ||
            opnd->reg == X86_REG_CH || opnd->reg == X86_REG_DH);
}
int ir1_opnd_is_8l(IR1_OPND *opnd);
int ir1_opnd_is_16l(IR1_OPND *opnd);
int ir1_opnd_is_gpr(IR1_OPND *opnd);
int ir1_opnd_is_uimm_within_16bit(IR1_OPND *opnd);
int ir1_opnd_is_simm_within_12bit(IR1_OPND *opnd);

longx ir1_opnd_u2simm(IR1_OPND *opnd);
ulongx ir1_opnd_s2uimm(IR1_OPND *opnd);
bool ir1_opnd_is_uimm12(IR1_OPND *opnd);
bool ir1_opnd_is_simm12(IR1_OPND *opnd);
bool ir1_opnd_is_s2uimm12(IR1_OPND *opnd);
bool ir1_opnd_is_u2simm12(IR1_OPND *opnd);

inline __attribute__ ((always_inline))
int ir1_opnd_is_mem(IR1_OPND *opnd) { return opnd->type == X86_OP_MEM; }
int ir1_opnd_is_fpr(IR1_OPND *opnd);
int ir1_opnd_is_seg(IR1_OPND *opnd);
int ir1_opnd_is_mmx(IR1_OPND *opnd);
int ir1_opnd_is_xmm(IR1_OPND *opnd);
int ir1_opnd_is_ymm(IR1_OPND *opnd);
int ir1_opnd_is_pc_relative(IR1_OPND *opnd);
int ir1_opnd_is_same_reg(IR1_OPND *opnd0, IR1_OPND *opnd1);
int ir1_opnd_is_same_reg_without_width(IR1_OPND *opnd0, IR1_OPND *opnd1);
inline __attribute__ ((always_inline))
int ir1_opnd_has_base(IR1_OPND *opnd)
{
    // may unnecessary to judge mem opnd
    return opnd->mem.base != X86_REG_INVALID;
}
inline __attribute__ ((always_inline))
int ir1_opnd_has_index(IR1_OPND *opnd)
{
    return opnd->mem.index != X86_REG_INVALID;
}
inline __attribute__ ((always_inline))
int ir1_opnd_has_seg(IR1_OPND *opnd)
{
    return opnd->mem.segment != X86_REG_INVALID;
}
int ir1_opnd_get_seg_index(IR1_OPND *opnd);
inline __attribute__ ((always_inline))
IR1_PREFIX ir1_prefix(IR1_INST *ir1)
{
    // TODO: one ins may has two or more PREFIX (rarely)
    // only support rep now
    return ir1->info->detail->x86.prefix[0];  // only lock rep
}
inline __attribute__ ((always_inline))
int ir1_opnd_num(IR1_INST *ir1) { return ir1->info->detail->x86.op_count; }
inline __attribute__ ((always_inline))
ADDRX ir1_addr(IR1_INST *ir1) { return ir1->info->address; }
inline __attribute__ ((always_inline))
ADDRX ir1_addr_next(IR1_INST *ir1)
{
    return ir1->info->address + ir1->info->size;
}
ADDRX ir1_target_addr(IR1_INST *ir1);
inline __attribute__ ((always_inline))
IR1_OPCODE ir1_opcode(IR1_INST *ir1) { return ir1->info->id; }

inline __attribute__ ((always_inline))
int ir1_is_branch(IR1_INST *ir1)
{
    switch (ir1->info->id) {
    case X86_INS_JO:
    case X86_INS_JNO:
    case X86_INS_JB:
    case X86_INS_JAE:
    case X86_INS_JE:
    case X86_INS_JNE:
    case X86_INS_JBE:
    case X86_INS_JA:
    case X86_INS_JS:
    case X86_INS_JNS:
    case X86_INS_JP:
    case X86_INS_JNP:
    case X86_INS_JL:
    case X86_INS_JGE:
    case X86_INS_JLE:
    case X86_INS_JG:

    case X86_INS_LOOPNE:
    case X86_INS_LOOPE:
    case X86_INS_LOOP:
    case X86_INS_JCXZ:
    case X86_INS_JECXZ:
    case X86_INS_JRCXZ:

        return 1;

    default:
        return 0;
    }
}
inline __attribute__ ((always_inline))
int ir1_is_jump(IR1_INST *ir1) { return ir1->info->id == X86_INS_JMP; }
inline __attribute__ ((always_inline))
int ir1_is_call(IR1_INST *ir1)
{
    // lsassert(0);
    // TODO : only direct?
    return ir1->info->id == X86_INS_CALL;
}
inline __attribute__ ((always_inline))
int ir1_is_return(IR1_INST *ir1)
{
#ifndef CONFIG_SOFTMMU
    return ir1->info->id == X86_INS_RET || ir1->info->id == X86_INS_IRET ||
           ir1->info->id == X86_INS_RETF;
#else
    /*
     * 'iret' and 'retf' is special EOB in sys-mode.
     * In current version, it depends on the added jmp.
     * Can be further optimized to use gen_exit_tb().
     * Take care of EIP, eflags, mapping registers, etc.
     */
    return ir1->info->id == X86_INS_RET;
#endif
}
inline __attribute__ ((always_inline))
int ir1_is_indirect(IR1_INST *ir1)
{
    lsassert(0);
    // TODO : some indirect call/jmp
    return ir1->info->id == X86_INS_LJMP || ir1->info->id == X86_INS_LCALL;
}
inline __attribute__ ((always_inline))
int ir1_is_syscall(IR1_INST *ir1)
{
    // TODO : 0x80?
    return ir1->info->id == X86_INS_INT;
    // X86_INS_SYSCALL,X86_INS_SYSENTER,X86_INS_SYSEXIT,X86_INS_SYSRET
}
bool ir1_is_tb_ending(IR1_INST *ir1);

inline __attribute__ ((always_inline))
bool ir1_is_cf_use(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_use, 1 << 0);
}
inline __attribute__ ((always_inline))
bool ir1_is_pf_use(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_use, 1 << 1);
}
inline __attribute__ ((always_inline))
bool ir1_is_af_use(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_use, 1 << 2);
}
inline __attribute__ ((always_inline))
bool ir1_is_zf_use(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_use, 1 << 3);
}
inline __attribute__ ((always_inline))
bool ir1_is_sf_use(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_use, 1 << 4);
}
inline __attribute__ ((always_inline))
bool ir1_is_of_use(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_use, 1 << 5);
}

inline __attribute__ ((always_inline))
bool ir1_is_cf_def(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_def, 1 << 0);
}
inline __attribute__ ((always_inline))
bool ir1_is_pf_def(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_def, 1 << 1);
}
inline __attribute__ ((always_inline))
bool ir1_is_af_def(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_def, 1 << 2);
}
inline __attribute__ ((always_inline))
bool ir1_is_zf_def(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_def, 1 << 3);
}
inline __attribute__ ((always_inline))
bool ir1_is_sf_def(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_def, 1 << 4);
}
inline __attribute__ ((always_inline))
bool ir1_is_of_def(IR1_INST *ir1)
{
    return BITS_ARE_SET(ir1->_eflag_def, 1 << 5);
}

inline __attribute__ ((always_inline))
uint8_t ir1_get_eflag_use(IR1_INST *ir1) { return ir1->_eflag_use; }
inline __attribute__ ((always_inline))
uint8_t ir1_get_eflag_def(IR1_INST *ir1) { return ir1->_eflag_def; }
inline __attribute__ ((always_inline))
void ir1_set_eflag_use(IR1_INST *ir1, uint8_t use) { ir1->_eflag_use = use; }
inline __attribute__ ((always_inline))
void ir1_set_eflag_def(IR1_INST *ir1, uint8_t def) { ir1->_eflag_def = def; }

int ir1_opnd_index_reg_num(IR1_OPND *opnd);
int ir1_opnd_base_reg_num(IR1_OPND *opnd);

#if defined(CONFIG_LATX_PRINT) || !defined(CONFIG_CAPSTONE_DIET)
int ir1_dump(IR1_INST *ir1);
int ir1_opcode_dump(IR1_INST *ir1);
#else
#define ir1_dump(ir1)
#define ir1_opcode_dump(ir1)
#endif
const char *ir1_name(IR1_OPCODE op);
const char *ir1_group_name(x86_insn_group grp);

bool ir1_need_calculate_of(IR1_INST *ir1);
bool ir1_need_calculate_cf(IR1_INST *ir1);
bool ir1_need_calculate_pf(IR1_INST *ir1);
bool ir1_need_calculate_af(IR1_INST *ir1);
bool ir1_need_calculate_zf(IR1_INST *ir1);
bool ir1_need_calculate_sf(IR1_INST *ir1);
bool ir1_need_calculate_any_flag(IR1_INST *ir1);

bool ir1_translate(IR1_INST *ir1);

inline __attribute__ ((always_inline))
uint8_t ir1_get_opnd_num(IR1_INST *ir1)
{
    return ir1->info->detail->x86.op_count;
}
inline __attribute__ ((always_inline))
IR1_OPND *ir1_get_opnd(IR1_INST *ir1, int i)
{
    lsassert(i <= ir1->info->detail->x86.op_count);
    return &(ir1->info->detail->x86.operands[i]);
}

inline __attribute__ ((always_inline))
uint8_t ir1_rex(IR1_INST *ir1)
{
    return ir1->info->detail->x86.rex;
}
    /*
     7                               0
     +---+---+---+---+---+---+---+---+
     | 0   1   0   0 | W | R | X | B |
     +---+---+---+---+---+---+---+---+
     */
inline __attribute__ ((always_inline))
uint8_t ir1_rex_w(IR1_INST *ir1)
{
    return ir1->info->detail->x86.rex & 0x8;
}

inline __attribute__ ((always_inline))
bool ir1_is_indirect_call(IR1_INST *ir1)
{
    return !ir1_opnd_is_imm(ir1_get_opnd(ir1, 0));
}
inline __attribute__ ((always_inline))
bool ir1_is_indirect_jmp(IR1_INST *ir1)
{
    return !ir1_opnd_is_imm(ir1_get_opnd(ir1, 0));
}
inline __attribute__ ((always_inline))
bool ir1_is_prefix_lock(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[0] == X86_PREFIX_LOCK;
}

inline __attribute__ ((always_inline))
bool ir1_is_indirect_inst(IR1_INST *ir1)
{
	if (!ir1_opnd_is_imm(ir1_get_opnd(ir1, 0))) {
		if (ir1->info->id == X86_INS_CALL ||
			ir1->info->id == X86_INS_JMP) {
			return true;
		}
	}
	if (ir1_opcode(ir1) == X86_INS_RET) {
		return true;
	}
	return false;
}

const char *ir1_reg_name(x86_reg reg);

ADDRX latxs_ir1_disasm(IR1_INST *ir1,
        uint8_t *addr, ADDRX t_pc, int *error, int ir1_num);

inline __attribute__ ((always_inline))
uint8_t *latxs_ir1_inst_opbytes(IR1_INST *ir1)
{
    return ir1->info->detail->x86.opcode;
}

int latxs_ir1_mem_opnd_is_base_imm(IR1_OPND *opnd);

int latxs_ir1_opnd_is_cr(IR1_OPND *opnd);
int latxs_ir1_opnd_is_dr(IR1_OPND *opnd);
int latxs_ir1_opnd_get_cr_num(IR1_OPND *opnd, int *flag);
int latxs_ir1_opnd_get_dr_num(IR1_OPND *opnd);

inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_lock(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[0] == X86_PREFIX_LOCK;
}
inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_seg(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[1] != 0;
}
inline __attribute__ ((always_inline))
int latxs_ir1_get_prefix_seg(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[1];
}
inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_opsize(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[2] == X86_PREFIX_OPSIZE;
}
inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_addrsize(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[3] == X86_PREFIX_ADDRSIZE;
}
inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_rep(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[0] == X86_PREFIX_REP;
}
inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_repe(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[0] == X86_PREFIX_REPE;
}
inline __attribute__ ((always_inline))
int latxs_ir1_has_prefix_repne(IR1_INST *ir1)
{
    return ir1->info->detail->x86.prefix[0] == X86_PREFIX_REPNE;
}

inline __attribute__ ((always_inline))
int latxs_ir1_is_lea(IR1_INST *ir1)
{
    return ir1->info->id == X86_INS_LEA;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_string_op(IR1_INST *ir1)
{
    uint8_t *opbytes = latxs_ir1_inst_opbytes(ir1);
    uint8_t opc = opbytes[0];
    return (0x6c <= opc && opc <= 0x6f) || /* ins/outs       */
           (0xa4 <= opc && opc <= 0xa7) || /* movs/cmps      */
           (0xaa <= opc && opc <= 0xaf) ;  /* stos/lods/scas */
}
static inline bool latxs_ir1_is_port_io(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_IN:
    case X86_INS_OUT:
    case X86_INS_INSB:
    case X86_INS_INSW:
    case X86_INS_INSD:
    case X86_INS_OUTSB:
    case X86_INS_OUTSW:
    case X86_INS_OUTSD:
        return true;
    default:
        return false;
    }
}

/* EOB instructions in system-mode */
int latxs_ir1_is_eob_in_sys(IR1_INST *ir1);
int latxs_ir1_is_cross_page(IR1_INST *ir1);
int latxs_ir1_is_cross_page_eip(IR1_INST *ir1, uint64_t csbase);
inline __attribute__ ((always_inline))
int latxs_ir1_is_jump_far(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_LJMP;
}
int latxs_ir1_is_mov_to_cr(IR1_INST *ir1);
int latxs_ir1_is_mov_to_cr8(IR1_INST *ir1);
int latxs_ir1_is_mov_from_cr(IR1_INST *ir1);
int latxs_ir1_is_mov_to_dr(IR1_INST *ir1);
int latxs_ir1_is_mov_to_seg(IR1_INST *ir1);
int latxs_ir1_is_pop_to_seg(IR1_INST *ir1);
inline __attribute__ ((always_inline))
int latxs_ir1_is_pop_eflags(IR1_INST *ir1)
{
    IR1_OPCODE opcode = ir1_opcode(ir1);
    return opcode == X86_INS_POPF  ||
           opcode == X86_INS_POPFD ||
           opcode == X86_INS_POPFQ ;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_rsm(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_RSM;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_sti(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_STI;
}
bool latxs_ir1_is_pop_to_seg_ss(IR1_INST *ir1);
bool latxs_ir1_is_mov_to_seg_ss(IR1_INST *ir1);
inline __attribute__ ((always_inline))
int latxs_ir1_is_nop(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_NOP;
}
int latxs_ir1_is_repz_nop(IR1_INST *ir1);
inline __attribute__ ((always_inline))
int latxs_ir1_is_iret(IR1_INST *ir1)
{
    IR1_OPCODE opcode = ir1_opcode(ir1);
    return opcode == X86_INS_IRET || opcode == X86_INS_IRETD ||
           opcode == X86_INS_IRETQ;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_lmsw(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_LMSW;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_wait(IR1_INST *ir1)
{
    IR1_OPCODE opcode = ir1_opcode(ir1);
    return opcode == X86_INS_WAIT;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_retf(IR1_INST *ir1)
{
    IR1_OPCODE opcode = ir1_opcode(ir1);
    return opcode == X86_INS_RETF || opcode == X86_INS_RETFQ;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_clts(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_CLTS;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_call_far(IR1_INST *ir1)
{
    return ir1->info->id == X86_INS_LCALL;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_invlpg(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_INVLPG;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_invlpga(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_INVLPGA;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_pause(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_PAUSE;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_sysenter(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_SYSENTER;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_sysexit(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_SYSEXIT;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_int(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_INT ||
            ir1_opcode(ir1) == X86_INS_INT1 ||
	        ir1_opcode(ir1) == X86_INS_INT3 ||
	        ir1_opcode(ir1) == X86_INS_INTO;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_syscall(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_SYSCALL;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_sysret(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_SYSRET;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_xrstor(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_XRSTOR;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_xsetbv(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_XSETBV;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_mwait(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_MWAIT;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_vmrun(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_VMRUN;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_stgi(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_STGI;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_wrmsr(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_WRMSR;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_clac(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_CLAC;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_stac(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_STAC;
}
/* EOB instructions that might change the state of FPU */
inline __attribute__ ((always_inline))
int latxs_ir1_contains_fldenv(IR1_INST *ir1)
{
    IR1_OPCODE opc = ir1_opcode(ir1);
    return opc == X86_INS_FLDENV  ||
           opc == X86_INS_FRSTOR  ||
           opc == X86_INS_FXRSTOR ||
           opc == X86_INS_XRSTOR;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_fninit(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_FNINIT;
}
inline __attribute__ ((always_inline))
int latxs_ir1_is_fnsave(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_FNSAVE;
}

void latxs_fix_up_ir1(IR1_INST *ir1);

inline __attribute__ ((always_inline))
void latxs_ir1_free_info(IR1_INST *ir1)
{
    if (ir1) {
        /* cs_free(ir1->info, ir1->info_count); */
    }
}

inline __attribute__ ((always_inline))
int latxs_ir1_is_illegal(IR1_INST *ir1)
{
    return ir1->info->id == X86_INS_INVALID;
}

int latxs_ir1_access_mmx(IR1_INST *ir1);

inline __attribute__ ((always_inline))
int latxs_ir1_inst_size(IR1_INST *ir1)
{
    return ir1->info->size;
}
inline __attribute__ ((always_inline))
int latxs_ir1_addr_size(IR1_INST *ir1)
{
    return ir1->info->detail->x86.addr_size;
}

int latxs_ir1_data_size(IR1_INST *ir1); /* Operand Size */

#define LATXS_IR1_FLAGS_ILLOP     1
#define LATXS_IR1_FLAGS_GENNOP    2
void latxs_ir1_make_ins_ILLEGAL(IR1_INST *ir1,
        ADDRX addr, int size, int flags);

#ifndef CONFIG_CAPSTONE_DIET
inline __attribute__ ((always_inline))
int ir1_grp_nr(IR1_INST *pir1)
{
    return pir1->info->detail->groups_count;
}
inline __attribute__ ((always_inline))
uint8_t *ir1_get_grps(IR1_INST *pir1)
{
    return pir1->info->detail->groups;
}
#endif

#endif
