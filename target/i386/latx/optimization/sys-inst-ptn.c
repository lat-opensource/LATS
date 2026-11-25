/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "ir1.h"
#include "lsenv.h"
#include "translate.h"
#include "latx-options.h"
#include "latx-sys-inst-ptn.h"
#include "tb-ir1.h"

#ifdef LATXS_INSTPTN_ENABLE

#define WRAP(ins) (X86_INS_##ins)

static int ir1_can_pattern(IR1_INST *pir1)
{
    switch (ir1_opcode(pir1)) {
    /*head*/
    case WRAP(CMP):
    case WRAP(CQO):
    case WRAP(XOR):
    case WRAP(CDQ):
    case WRAP(TEST):
    case WRAP(UCOMISD):
    /*tail*/
    case WRAP(SBB):
    case WRAP(IDIV):
    case WRAP(DIV):
    case WRAP(SETB):
    case WRAP(SETAE):
    case WRAP(SETE):
    case WRAP(SETNE):
    case WRAP(SETBE):
    case WRAP(SETA):
    case WRAP(SETL):
    case WRAP(SETGE):
    case WRAP(SETLE):
    case WRAP(SETG):
    case WRAP(SETS):
    case WRAP(SETNS):
    case WRAP(SETNO):
    case WRAP(SETO):
    case WRAP(CMOVE):
    case WRAP(CMOVNE):
    case WRAP(CMOVS):
    case WRAP(CMOVNS):
    case WRAP(CMOVLE):
    case WRAP(CMOVG):
    case WRAP(CMOVNO):
    case WRAP(CMOVO):
    case WRAP(CMOVB):
    case WRAP(CMOVBE):
    case WRAP(CMOVA):
    case WRAP(CMOVAE):
    case WRAP(CMOVL):
    case WRAP(CMOVGE):
        return 1;
    default:
        return 0;
    }
}

static int ir1_is_pattern_head(IR1_INST *pir1)
{
    switch (ir1_opcode(pir1)) {
    case WRAP(CMP):
    case WRAP(CQO):
    case WRAP(XOR):
    case WRAP(CDQ):
    case WRAP(TEST):
    case WRAP(UCOMISD):
        return 1;
    default:
        return 0;
    }
}

#define SCAN_CHECK(buf, i) do { \
    if (buf[i] == -1) return false; \
} while (0)
#define SCAN_IDX(buf, i)        (buf[i])
#define SCAN_IR1(list, buf, i)    (ir1_inst_in_list(list, SCAN_IDX(buf, i)))

typedef int scan_elem_t;

static inline void scan_clear(scan_elem_t *scan)
{
    if (scan[0] == -1) return;
    memset(scan, -1, sizeof(scan_elem_t) * INSTPTN_BUF_SIZE);
}

static inline void scan_push(scan_elem_t *scan, int pir1_index)
{
    for(int i = INSTPTN_BUF_SIZE - 1; i > 0; --i) {
        scan[i] = scan[i-1];
    }
    scan[0] = pir1_index;
}

static bool is_contain_edx(IR1_OPND *opnd)
{
    if (ir1_opnd_is_gpr(opnd)) {
        switch (opnd->reg) {
        case X86_REG_DL:    case X86_REG_DH:
        case X86_REG_DX:    case X86_REG_EDX:
        case X86_REG_RDX:
            return true;
        default:
            break;
        }
    } else if (ir1_opnd_is_mem(opnd)) {
        switch (opnd->mem.base) {
        case X86_REG_DL:    case X86_REG_DH:
        case X86_REG_DX:    case X86_REG_EDX:
        case X86_REG_RDX:
            return true;
        default:
            break;
        }
        switch (opnd->mem.index) {
        case X86_REG_DL:    case X86_REG_DH:
        case X86_REG_DX:    case X86_REG_EDX:
        case X86_REG_RDX:
            return true;
        default:
            break;
        }
    }
    return false;
}

static int inst_pattern(TranslationBlock *tb,
        IR1_INST *pir1, scan_elem_t *scan)
{
    IR1_INST *ir1 = NULL;
    IR1_OPND *opnd0 = NULL;
    IR1_OPND *opnd1 = NULL;

    TRANSLATION_DATA *td = lsenv->tr_data;
    IR1_INST *list = td->ir1_inst_array;
    /*
     * pir1 is pattern head
     * scan[] contains ir1 following the head
     */
    switch (ir1_opcode(pir1)) {
    case WRAP(CMP): {
        SCAN_CHECK(scan, 0);
        ir1 = SCAN_IR1(list, scan, 0);
        if(ir1_opcode(ir1) == WRAP(SBB)) {
            latxs_instptn_check_cmp_sbb_0();

            opnd0 = ir1_get_opnd(ir1, 0);
            opnd1 = ir1_get_opnd(ir1, 1);
            if (!ir1_opnd_is_same_reg(opnd0, opnd1)) {
                return 0;
            }
            pir1->instptn.opc  = INSTPTN_OPC_CMP_SBB;
            pir1->instptn.next = SCAN_IDX(scan, 0);
            ir1->instptn.opc  = INSTPTN_OPC_NOP;
            ir1->instptn.next = -1;
            return 1;
        }
        latxs_instptn_check_cmp_xxcc_con_0();
        IR1_INST *curr = ir1;
        IR1_INST *prev = pir1;
        for (int index = 0; index < INSTPTN_BUF_SIZE && scan[index] >=0; index++) {
            curr = SCAN_IR1(list, scan, index);
            switch (ir1_opcode(curr)) {
            case WRAP(SETB):
            case WRAP(SETAE):
            case WRAP(SETE):
            case WRAP(SETNE):
            case WRAP(SETBE):
            case WRAP(SETA):
            case WRAP(SETL):
            case WRAP(SETGE):
            case WRAP(SETLE):
            case WRAP(SETG):
            case WRAP(CMOVB):
            case WRAP(CMOVAE):
            case WRAP(CMOVE):
            case WRAP(CMOVNE):
            case WRAP(CMOVBE):
            case WRAP(CMOVA):
            case WRAP(CMOVL):
            case WRAP(CMOVGE):
            case WRAP(CMOVLE):
            case WRAP(CMOVG):
                curr->_eflag_opt = IR1_EFOPT_NONE;
                if (index == 0) {
                    prev->instptn.opc  = INSTPTN_OPC_CMP_XXCC_CON;
                } else {
                    prev->instptn.opc  = INSTPTN_OPC_NOP_FOR_EXP;
                }
                prev->instptn.next = SCAN_IDX(scan, index);
                curr->instptn.opc  = INSTPTN_OPC_NOP_FOR_EXP;
                curr->instptn.next = -1;
                prev = curr;
                break;
            default:
                return 1;
            }
        }
        return 1;
    }
    case WRAP(TEST): {
        SCAN_CHECK(scan, 0);
        latxs_instptn_check_test_xxcc_con_0();
        IR1_INST *curr = ir1;
        IR1_INST *prev = pir1;
        for (int index = 0; index < INSTPTN_BUF_SIZE && scan[index] >=0; index++) {
            curr = SCAN_IR1(list, scan, index);
            switch (ir1_opcode(curr)) {
            case WRAP(SETS):
            case WRAP(SETNS):
            case WRAP(SETLE):
            case WRAP(SETG):
            case WRAP(CMOVS):
            case WRAP(CMOVNS):
            case WRAP(CMOVLE):
            case WRAP(CMOVG):
                opnd0 = ir1_get_opnd(pir1, 0);
                opnd1 = ir1_get_opnd(pir1, 1);
                if (!ir1_opnd_is_same_reg(opnd0, opnd1)) {
                    return 0;
                }
                __attribute__((fallthrough));
            case WRAP(SETE):
            case WRAP(SETNE):
            case WRAP(SETNO):
            case WRAP(SETO):
            case WRAP(SETB):
            case WRAP(SETBE):
            case WRAP(SETA):
            case WRAP(SETAE):
            case WRAP(CMOVE):
            case WRAP(CMOVNE):
            case WRAP(CMOVNO):
            case WRAP(CMOVO):
            case WRAP(CMOVB):
            case WRAP(CMOVBE):
            case WRAP(CMOVA):
            case WRAP(CMOVAE):
                curr->_eflag_opt = IR1_EFOPT_NONE;
                if (index == 0) {
                    prev->instptn.opc  = INSTPTN_OPC_TEST_XXCC_CON;
                } else {
                    prev->instptn.opc  = INSTPTN_OPC_NOP_FOR_EXP;
                }
                prev->instptn.next = SCAN_IDX(scan, index);
                curr->instptn.opc  = INSTPTN_OPC_NOP_FOR_EXP;
                curr->instptn.next = -1;
                prev = curr;
                break;
            default:
                return 1;
            }
        }
        return 1;
    }
    case WRAP(CQO):
        SCAN_CHECK(scan, 0);
        latxs_instptn_check_cqo_idiv_0();

        ir1 = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1)) {
        case WRAP(IDIV):
            opnd0 = ir1_get_opnd(ir1, 0);
            if (ir1_opnd_size(opnd0) != 64)
                return 0;
            if (is_contain_edx(opnd0))
                return 0;
            // if (ir1_opnd_is_mem(ir1_get_opnd(ir1, 0))) {
            //     return 0;
            // }
            pir1->instptn.opc  = INSTPTN_OPC_CQO_IDIV;
            pir1->instptn.next = SCAN_IDX(scan, 0);
            ir1->instptn.opc  = INSTPTN_OPC_NOP;
            ir1->instptn.next = -1;
            return 1;
        default:
            return 0;
        }
    case WRAP(XOR):
        SCAN_CHECK(scan, 0);
        latxs_instptn_check_xor_div_0();

        opnd0 = ir1_get_opnd(pir1, 0);
        opnd1 = ir1_get_opnd(pir1, 1);

        ir1 = SCAN_IR1(list, scan, 0);
        if (ir1_opcode(ir1) == WRAP(DIV)) {
            if (ir1_opnd_is_gpr(opnd0) && ir1_opnd_is_gpr(opnd1) &&
                ((opnd0->reg == X86_REG_EDX && opnd1->reg == X86_REG_EDX &&
                ir1_opnd_size(ir1_get_opnd(ir1, 0)) == 32 &&
                ir1_opnd_is_gpr(ir1_get_opnd(ir1, 0))) ||
                (opnd0->reg == X86_REG_RDX && opnd1->reg == X86_REG_RDX &&
                ir1_opnd_size(ir1_get_opnd(ir1, 0)) == 64 &&
                ir1_opnd_is_gpr(ir1_get_opnd(ir1, 0))))) {
                    if (is_contain_edx(opnd0))
                        return 0;
                    pir1->instptn.opc  = INSTPTN_OPC_XOR_DIV;
                    pir1->instptn.next = SCAN_IDX(scan, 0);
                    ir1->instptn.opc  = INSTPTN_OPC_NOP;
                    ir1->instptn.next = -1;
                    return 1;
                }
        }
        return 0;
    case WRAP(CDQ):
        SCAN_CHECK(scan, 0);
        latxs_instptn_check_cdq_idiv_0();

        ir1 = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1)) {
        case WRAP(IDIV):
            opnd0 = ir1_get_opnd(ir1, 0);
            if (ir1_opnd_size(opnd0) != 32)
                return 0;
            if (is_contain_edx(opnd0))
                return 0;
            // if (ir1_opnd_is_mem(opnd0)) {
            //     return 0;
            // }
            pir1->instptn.opc  = INSTPTN_OPC_CDQ_IDIV;
            pir1->instptn.next = SCAN_IDX(scan, 0);
            ir1->instptn.opc  = INSTPTN_OPC_NOP;
            ir1->instptn.next = -1;
            return 1;
        default:
            return 0;
        }
    case WRAP(UCOMISD):
        SCAN_CHECK(scan, 0);
        if (tb->flags & (HF_TS_MASK | HF_EM_MASK)) {
            return 0;
        }
        latxs_instptn_check_ucomisd_seta_0();

        ir1 = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1)) {
        case WRAP(SETA):
            pir1->instptn.opc  = INSTPTN_OPC_UCOMISD_SETA;
            pir1->instptn.next = SCAN_IDX(scan, 0);
            ir1->instptn.opc  = INSTPTN_OPC_NOP;
            ir1->instptn.next = -1;
            return 1;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

static void inst_pattern_scan_con(TranslationBlock *tb,
        scan_elem_t *scan,
        IR1_INST *pir1, int pir1_index)
{
    if (!ir1_can_pattern(pir1)) {
        scan_clear(scan);
        return;
    }

    if (!ir1_is_pattern_head(pir1)) {
        scan_push(scan, pir1_index);
        return;
    }

    /* pir1 must be pattern head */
    if (inst_pattern(tb, pir1, scan)) {
        scan_clear(scan);
    } else {
        scan_push(scan, pir1_index);
    }
}

#if 0
bool break_pattern(IR1_OPCODE op)
{
    switch (op) {
        case WRAP(AAA):
        case WRAP(AAD):
        case WRAP(AAM):
        case WRAP(AAS):
        case WRAP(ADC):
        case WRAP(ADD):
        case WRAP(AND):
        case WRAP(ANDN):
        case WRAP(ARPL):
        case WRAP(BLSI):
        case WRAP(BLSR):
        case WRAP(BLSMSK):
        case WRAP(BSF):
        case WRAP(BSR):
        case WRAP(BTC):
        case WRAP(BTR):
        case WRAP(BTS):
        case WRAP(BZHI):
        case WRAP(CLC):
        case WRAP(CLD):
        case WRAP(SALC):
        case WRAP(CMC):
        /* CMOVcc */
        case WRAP(CMOVA):
        case WRAP(CMOVAE):
        case WRAP(CMOVB):
        case WRAP(CMOVBE):
        case WRAP(CMOVE):
        case WRAP(CMOVG):
        case WRAP(CMOVGE):
        case WRAP(CMOVL):
        case WRAP(CMOVLE):
        case WRAP(CMOVNE):
        case WRAP(CMOVO):
        case WRAP(CMOVNO):
        case WRAP(CMOVP):
        case WRAP(CMOVNP):
        case WRAP(CMOVS):
        case WRAP(CMOVNS):

        case WRAP(CMPSB):
        case WRAP(CMPSW):
        case WRAP(CMPSD):
        case WRAP(CMPSQ):
        case WRAP(CMPXCHG):
        case WRAP(CMPXCHG8B):
        case WRAP(CMPXCHG16B):
        case WRAP(COMISD):
        case WRAP(COMISS):
        case WRAP(DAA):
        case WRAP(DAS):
        case WRAP(DEC):
        case WRAP(DIV):
        /* FCMOVcc */
        case WRAP(FCMOVB):
        case WRAP(FCMOVBE):
        case WRAP(FCMOVE):
        case WRAP(FCMOVNB):
        case WRAP(FCMOVNBE):
        case WRAP(FCMOVNE):
        case WRAP(FCMOVNU):
        case WRAP(FCMOVU):

        case WRAP(FCOMI):
        case WRAP(FCOMIP):
        case WRAP(FUCOMI):
        case WRAP(FUCOMIP):
        case WRAP(IDIV):
        case WRAP(IMUL):
        case WRAP(INC):
        case WRAP(INSB):
        case WRAP(INSW):
        case WRAP(INSD):
        case WRAP(INTO):
        case WRAP(UCOMISD):
        case WRAP(UCOMISS):
        case WRAP(LAHF):
        /* lods */
        case WRAP(LODSB):
        case WRAP(LODSW):
        case WRAP(LODSD):
        case WRAP(LODSQ):
        case WRAP(LOOPE):
        case WRAP(LOOPNE):
        case WRAP(LZCNT):
        /* movs */
        case WRAP(MOVSB):
        case WRAP(MOVSW):
        case WRAP(MOVSD):
        case WRAP(MOVSQ):

        case WRAP(MUL):
        case WRAP(NEG):
        case WRAP(OR):
        case WRAP(OUTSB):
        case WRAP(OUTSW):
        case WRAP(OUTSD):
        case WRAP(POPF):
        case WRAP(POPFD):
        case WRAP(POPFQ):
        case WRAP(PUSHF):
        case WRAP(PUSHFD):
        case WRAP(PUSHFQ):
        case WRAP(RCL):
        case WRAP(RCR):

        case WRAP(ROL):
        case WRAP(ROR):
        case WRAP(RSM):
        case WRAP(SAHF):
        case WRAP(SAL):
        case WRAP(SAR):
        case WRAP(SHL):
        case WRAP(SHR):
        case WRAP(SBB):
        /* scas */
        case WRAP(SCASB):
        case WRAP(SCASW):
        case WRAP(SCASD):
        case WRAP(SCASQ):
        /* SETcc */
        case WRAP(SETO):
        case WRAP(SETNO):
        case WRAP(SETB):
        case WRAP(SETAE):
        case WRAP(SETE):
        case WRAP(SETNE):
        case WRAP(SETBE):
        case WRAP(SETA):
        case WRAP(SETS):
        case WRAP(SETNS):
        case WRAP(SETP):
        case WRAP(SETNP):
        case WRAP(SETL):
        case WRAP(SETGE):
        case WRAP(SETLE):
        case WRAP(SETG):
        /* INVALID */
        case WRAP(SHLD):
        case WRAP(SHRD):
        case WRAP(STC):
        case WRAP(STD):
        case WRAP(SUB):
        case WRAP(TZCNT):
        case WRAP(VERR):
        case WRAP(VERW):
        case WRAP(XADD):
        case WRAP(XOR):
        case WRAP(ADCX):
        case WRAP(ADOX):
            return false;
        default:
            return true;
    }
}
#endif

static bool inst_pattern_scan_jcc_end(TranslationBlock *tb,
        scan_elem_t *scan,
        IR1_INST *pir1, int pir1_index)
{
    if (tb->icount - 1 == pir1_index)   /*jcc*/
    {
        if (!pir1_index) return false; /* tb->icount > 1*/
        switch (ir1_opcode(pir1)) {
        case WRAP(JA):
        case WRAP(JAE):
        case WRAP(JB):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JBE):
        case WRAP(JL):
        case WRAP(JGE):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JNO):
        case WRAP(JO):
        case WRAP(JS):
        case WRAP(JNS):
            scan[0] = pir1_index;
            return true;
        default:
            return false;
        }
    }

    TRANSLATION_DATA *td = lsenv->tr_data;
    IR1_INST *list = td->ir1_inst_array;
    IR1_INST *ir1_jcc = NULL;
    IR1_OPND *opnd0 = NULL;
    IR1_OPND *opnd1 = NULL;
    switch (ir1_opcode(pir1)) {
    case WRAP(CMP):
        SCAN_CHECK(scan, 0);
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JB):
        case WRAP(JAE):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JBE):
        case WRAP(JA):
        case WRAP(JL):
        case WRAP(JGE):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JS):
        case WRAP(JNS):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_cmp_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_CMP_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_cmp_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_CMP_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_CMP_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(TEST):
        SCAN_CHECK(scan, 0);
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JS):
        case WRAP(JNS):
        case WRAP(JLE):
        case WRAP(JG):
            opnd0 = ir1_get_opnd(pir1, 0);
            opnd1 = ir1_get_opnd(pir1, 1);
            if (!ir1_opnd_is_same_reg(opnd0, opnd1)) {
                return false;
            }
            __attribute__((fallthrough));
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JNO):
        case WRAP(JO):
        case WRAP(JB):
        case WRAP(JBE):
        case WRAP(JA):
        case WRAP(JAE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_test_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_TEST_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_test_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_TEST_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_TEST_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(AND):
        SCAN_CHECK(scan, 0);
        if (latxs_ir1_has_prefix_lock(pir1) &&
            (lsenv->tr_data->sys.cflags & CF_PARALLEL))
            return false;
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JS):
        case WRAP(JNS):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JNO):
        case WRAP(JO):
        case WRAP(JB):
        case WRAP(JBE):
        case WRAP(JA):
        case WRAP(JAE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_and_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_AND_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else if (ir1_opcode(ir1_jcc) == WRAP(JE) ||
                       ir1_opcode(ir1_jcc) == WRAP(JNE)) {
                latxs_instptn_check_and_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_AND_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_AND_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(OR):
        SCAN_CHECK(scan, 0);
        if (latxs_ir1_has_prefix_lock(pir1) &&
            (lsenv->tr_data->sys.cflags & CF_PARALLEL))
            return false;
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JS):
        case WRAP(JNS):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JNO):
        case WRAP(JO):
        case WRAP(JB):
        case WRAP(JBE):
        case WRAP(JA):
        case WRAP(JAE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_or_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_OR_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_or_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_OR_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_OR_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(XOR):
        SCAN_CHECK(scan, 0);
        if (latxs_ir1_has_prefix_lock(pir1) &&
            (lsenv->tr_data->sys.cflags & CF_PARALLEL))
            return false;
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JS):
        case WRAP(JNS):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JNO):
        case WRAP(JO):
        case WRAP(JB):
        case WRAP(JBE):
        case WRAP(JA):
        case WRAP(JAE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_xor_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_XOR_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_xor_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_XOR_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_XOR_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(BT):
        SCAN_CHECK(scan, 0);
        if (td->opt_mmio && td->opt_mmio_type == TB_OPT_MMIO_APIC) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JB):
        case WRAP(JAE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_bt_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_BT_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_bt_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_BT_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_BT_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(COMISD):
        SCAN_CHECK(scan, 0);
        if (tb->flags & (HF_TS_MASK | HF_EM_MASK)) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JA):
        case WRAP(JAE):
        case WRAP(JB):
        case WRAP(JBE):
        case WRAP(JNE):
        case WRAP(JE):
        case WRAP(JL):
        case WRAP(JGE):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JP):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_comisd_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_COMISD_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_comisd_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_COMISD_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_COMISD_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(COMISS):
        SCAN_CHECK(scan, 0);
        if (tb->flags & (HF_TS_MASK | HF_EM_MASK)) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JA):
        case WRAP(JAE):
        case WRAP(JB):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JBE):
        case WRAP(JL):
        case WRAP(JGE):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JP):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_comiss_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_COMISS_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_comiss_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_COMISS_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_COMISS_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(UCOMISD):
        SCAN_CHECK(scan, 0);
        if (tb->flags & (HF_TS_MASK | HF_EM_MASK)) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JA):
        case WRAP(JAE):
        case WRAP(JB):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JBE):
        case WRAP(JL):
        case WRAP(JGE):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JP):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_ucomisd_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_UCOMISD_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_ucomisd_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_UCOMISD_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_UCOMISD_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(UCOMISS):
        SCAN_CHECK(scan, 0);
        if (tb->flags & (HF_TS_MASK | HF_EM_MASK)) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JA):
        case WRAP(JAE):
        case WRAP(JB):
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JBE):
        case WRAP(JL):
        case WRAP(JGE):
        case WRAP(JLE):
        case WRAP(JG):
        case WRAP(JP):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_ucomiss_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_UCOMISS_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else {
                latxs_instptn_check_ucomiss_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_UCOMISS_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_UCOMISS_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(DEC):
        SCAN_CHECK(scan, 0);
        if (latxs_ir1_has_prefix_lock(pir1) ||
            !ir1_opnd_is_gpr(ir1_get_opnd(pir1, 0))) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JE):
        case WRAP(JNE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_dec_jcc_0();
                pir1->instptn.opc = INSTPTN_OPC_DEC_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            }
            return false;
        default:
            return false;
        }
    case WRAP(ADD):
        SCAN_CHECK(scan, 0);
        if (latxs_ir1_has_prefix_lock(pir1) &&
            (lsenv->tr_data->sys.cflags & CF_PARALLEL))
            return false;
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JNS):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_add_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_ADD_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            }
            return false;
        default:
            return false;
        }
    case WRAP(SUB):
        SCAN_CHECK(scan, 0);
        if (latxs_ir1_has_prefix_lock(pir1) &&
            (lsenv->tr_data->sys.cflags & CF_PARALLEL))
            return false;
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JS):
        case WRAP(JNS):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_sub_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_SUB_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else if ((ir1_opcode(ir1_jcc) == WRAP(JS) ||
                        ir1_opcode(ir1_jcc) == WRAP(JNS)) &&
                       ir1_opnd_is_gpr(ir1_get_opnd(pir1, 0)) &&
                       (ir1_opnd_is_gpr(ir1_get_opnd(pir1, 1)) ||
                        ir1_opnd_is_imm(ir1_get_opnd(pir1, 1)))) {
                latxs_instptn_check_sub_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_SUB_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_SUB_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    case WRAP(SAR): {
        SCAN_CHECK(scan, 0);
        opnd0 = ir1_get_opnd(pir1, 0);
        opnd1 = ir1_get_opnd(pir1, 1);
        if (!ir1_opnd_is_imm(opnd1)) {
            return false;
        }
        int shift_mask = 0x1f;
        if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
            shift_mask = 0x3f;
        }
        if ((ir1_opnd_uimm(opnd1) & shift_mask) == 0) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JE):
        case WRAP(JNE):
        case WRAP(JS):
        case WRAP(JNS):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_sar_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_SAR_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            }
            return false;
        default:
            return false;
        }
    }
    case WRAP(SHR): {
        SCAN_CHECK(scan, 0);
        opnd0 = ir1_get_opnd(pir1, 0);
        opnd1 = ir1_get_opnd(pir1, 1);
        if (!ir1_opnd_is_imm(opnd1)) {
            return false;
        }
        int shift_mask = 0x1f;
        if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
            shift_mask = 0x3f;
        }
        if ((ir1_opnd_uimm(opnd1) & shift_mask) == 0) {
            return false;
        }
        ir1_jcc = SCAN_IR1(list, scan, 0);
        switch (ir1_opcode(ir1_jcc)) {
        case WRAP(JE):
        case WRAP(JNE):
            if (pir1_index + 1 == SCAN_IDX(scan, 0)) {
                latxs_instptn_check_shr_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_SHR_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_NOP;
                ir1_jcc->instptn.next = -1;
            } else if (ir1_opcode(ir1_jcc) == WRAP(JE)) {
                latxs_instptn_check_shr_xx_jcc_0();
                pir1->instptn.opc  = INSTPTN_OPC_SHR_XX_JCC;
                pir1->instptn.next = SCAN_IDX(scan, 0);
                ir1_jcc->instptn.opc  = INSTPTN_OPC_SHR_XX_JCC;
                ir1_jcc->instptn.next = pir1_index;
            }
            return false;
        default:
            return false;
        }
    }
    case WRAP(MOV):
        if (td->opt_mmio) {
            return false;
        }
        opnd0 = ir1_get_opnd(pir1, 0);
        opnd1 = ir1_get_opnd(pir1, 1);
        return (ir1_opnd_is_gpr(opnd0) || ir1_opnd_is_mem(opnd0)) &&
               (ir1_opnd_is_gpr(opnd1) || ir1_opnd_is_mem(opnd1) ||
                ir1_opnd_is_imm(opnd1));
    case WRAP(MOVSD):
        if (pir1->info->detail->x86.opcode[0] == 0xa5) {
            return false;
        }
        return !(tb->flags & (HF_TS_MASK | HF_EM_MASK));
    case WRAP(ADDSD):
    case WRAP(ADDSS):
    case WRAP(MOVAPD):
    case WRAP(MOVHPS):
    case WRAP(MOVLPS):
    case WRAP(MOVSS):
    case WRAP(MULPS):
    case WRAP(MULSD):
    case WRAP(MULSS):
    case WRAP(PSHUFD):
    case WRAP(PUNPCKLWD):
        return !(tb->flags & (HF_TS_MASK | HF_EM_MASK));
    case WRAP(LEA):
    case WRAP(MOVSX):
    case WRAP(MOVSXD):
    case WRAP(MOVZX):
    case WRAP(NOP):
    case WRAP(PUSH):
        return true;
    default:
        return false;
            //return break_pattern(ir1_opcode(pir1));
    }
}

void __latxs_inst_pattern(TranslationBlock *tb)
{
    IR1_INST *ir1 = NULL;
    TRANSLATION_DATA *td = lsenv->tr_data;
    IR1_INST *list = td->ir1_inst_array;

#ifdef CONFIG_LATX_DEBUG
    for (int j = 0; j < tb_ir1_num(tb); ++j) {
        ir1 = ir1_inst_in_list(list, j);
        if (ir1->instptn.opc != INSTPTN_OPC_NONE ||
            ir1->instptn.next != -1) {
            fprintf(stderr,
                    "latxs instptn: stale marker before rescan: "
                    "tb_pc=0x%llx index=%d/%d opc=0x%llx next=%d\n",
                    (unsigned long long)tb->pc, j, tb_ir1_num(tb),
                    (unsigned long long)ir1->instptn.opc,
                    ir1->instptn.next);
            ir1->instptn.opc = INSTPTN_OPC_NONE;
            ir1->instptn.next = -1;
        }
    }
#endif

    latxs_instptn_check_void();
    latxs_instptn_cpl_filter_void(tb);

    scan_elem_t scan_con[INSTPTN_BUF_SIZE] = {0};
    scan_clear(scan_con);
    scan_elem_t scan_jcc_end[1] = {-1};

    int i = tb_ir1_num(tb) - 1;
    bool scan_head = true;
    for (; i >= 0; --i) {
        ir1 = ir1_inst_in_list(list, i);
        inst_pattern_scan_con(tb, scan_con, ir1, i);
        if (scan_head)
            scan_head = inst_pattern_scan_jcc_end(tb, scan_jcc_end, ir1, i);
    }
}

#endif
