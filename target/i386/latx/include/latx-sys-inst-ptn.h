/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_INSTPTN_H_
#define _LATX_SYS_INSTPTN_H_

#define LATXS_INSTPTN_ENABLE
#define LATXS_INSTPTN_OPTION_CHECK
#define LATXS_INSTPTN_CPL_FILTER



#ifdef LATXS_INSTPTN_ENABLE

#define INSTPTN_BUF_SIZE    4

#define INSTPTN_OPC_NONE        0x000000
#define INSTPTN_OPC_NOP         0x000001
#define INSTPTN_OPC_NOP_FOR_EXP 0x000002
#define INSTPTN_OPC_CMP_JCC     0x000010
#define INSTPTN_OPC_TEST_JCC    0x000020
#define INSTPTN_OPC_BT_JCC      0x000040
#define INSTPTN_OPC_CQO_IDIV    0x000080
#define INSTPTN_OPC_CMP_SBB     0x000100
#define INSTPTN_OPC_COMISD_JCC  0x000200
#define INSTPTN_OPC_COMISS_JCC  0x000400
#define INSTPTN_OPC_UCOMISD_JCC 0x000800
#define INSTPTN_OPC_UCOMISS_JCC 0x001000
#define INSTPTN_OPC_XOR_DIV     0x002000
#define INSTPTN_OPC_CDQ_IDIV    0x004000

#define INSTPTN_OPC_CMP_XX_JCC     0x008000
#define INSTPTN_OPC_TEST_XX_JCC    0x010000
#define INSTPTN_OPC_BT_XX_JCC      0x020000
#define INSTPTN_OPC_COMISD_XX_JCC  0x040000
#define INSTPTN_OPC_COMISS_XX_JCC  0x080000
#define INSTPTN_OPC_UCOMISD_XX_JCC 0x100000
#define INSTPTN_OPC_UCOMISS_XX_JCC 0x200000

#define INSTPTN_OPC_CMP_XXCC_CON    0x400000
#define INSTPTN_OPC_TEST_XXCC_CON   0x800000

#define INSTPTN_OPC_UCOMISD_SETA   0x1000000
#define INSTPTN_OPC_ADD_JCC        0x2000000
#define INSTPTN_OPC_SUB_JCC        0x4000000
#define INSTPTN_OPC_AND_JCC        0x8000000
#define INSTPTN_OPC_OR_JCC         0x10000000U
#define INSTPTN_OPC_OR_XX_JCC      0x20000000U
#define INSTPTN_OPC_XOR_JCC        0x40000000U
#define INSTPTN_OPC_XOR_XX_JCC     0x80000000U
#define INSTPTN_OPC_SAR_JCC        0x100000000ULL
#define INSTPTN_OPC_AND_XX_JCC     0x200000000ULL
#define INSTPTN_OPC_SHR_JCC        0x400000000ULL
#define INSTPTN_OPC_SUB_XX_JCC     0x800000000ULL
#define INSTPTN_OPC_SHR_XX_JCC     0x1000000000ULL
#define INSTPTN_OPC_DEC_JCC        0x2000000000ULL

#define latxs_inst_pattern(tb) do { \
      __latxs_inst_pattern(tb);     \
} while (0)
void __latxs_inst_pattern(TranslationBlock *tb);

bool try_translate_instptn(IR1_INST *pir1);

#ifdef LATXS_INSTPTN_OPTION_CHECK
#define latxs_instptn_check_void() do { \
    if (!option_instptn) return;        \
} while (0)
#define latxs_instptn_check_false() do {    \
    if (!option_instptn) return false;      \
} while (0)

#define LATXS_INSTPTN_CHECK_XX_0(OPC) do {   \
    if (!(option_instptn & (INSTPTN_OPC_##OPC >> 4))) return 0;      \
} while (0)

#define latxs_instptn_check_cmp_jcc_0() LATXS_INSTPTN_CHECK_XX_0(CMP_JCC)
#define latxs_instptn_check_test_jcc_0() LATXS_INSTPTN_CHECK_XX_0(TEST_JCC)
#define latxs_instptn_check_bt_jcc_0() LATXS_INSTPTN_CHECK_XX_0(BT_JCC)
#define latxs_instptn_check_cqo_idiv_0() LATXS_INSTPTN_CHECK_XX_0(CQO_IDIV)
#define latxs_instptn_check_cmp_sbb_0() LATXS_INSTPTN_CHECK_XX_0(CMP_SBB)
#define latxs_instptn_check_comisd_jcc_0() LATXS_INSTPTN_CHECK_XX_0(COMISD_JCC)
#define latxs_instptn_check_comiss_jcc_0() LATXS_INSTPTN_CHECK_XX_0(COMISS_JCC)
#define latxs_instptn_check_ucomisd_jcc_0() LATXS_INSTPTN_CHECK_XX_0(UCOMISD_JCC)
#define latxs_instptn_check_ucomiss_jcc_0() LATXS_INSTPTN_CHECK_XX_0(UCOMISS_JCC)
#define latxs_instptn_check_xor_div_0() LATXS_INSTPTN_CHECK_XX_0(XOR_DIV)
#define latxs_instptn_check_cdq_idiv_0() LATXS_INSTPTN_CHECK_XX_0(CDQ_IDIV)

#define latxs_instptn_check_cmp_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(CMP_XX_JCC)
#define latxs_instptn_check_test_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(TEST_XX_JCC)
#define latxs_instptn_check_bt_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(BT_XX_JCC)
#define latxs_instptn_check_comisd_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(COMISD_XX_JCC)
#define latxs_instptn_check_comiss_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(COMISS_XX_JCC)
#define latxs_instptn_check_ucomisd_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(UCOMISD_XX_JCC)
#define latxs_instptn_check_ucomiss_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(UCOMISS_XX_JCC)

#define latxs_instptn_check_cmp_xxcc_con_0() LATXS_INSTPTN_CHECK_XX_0(CMP_XXCC_CON)
#define latxs_instptn_check_test_xxcc_con_0() LATXS_INSTPTN_CHECK_XX_0(TEST_XXCC_CON)

#define latxs_instptn_check_ucomisd_seta_0() LATXS_INSTPTN_CHECK_XX_0(UCOMISD_SETA)

#define latxs_instptn_check_add_jcc_0() LATXS_INSTPTN_CHECK_XX_0(ADD_JCC)
#define latxs_instptn_check_sub_jcc_0() LATXS_INSTPTN_CHECK_XX_0(SUB_JCC)
#define latxs_instptn_check_and_jcc_0() LATXS_INSTPTN_CHECK_XX_0(AND_JCC)
#define latxs_instptn_check_and_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(AND_XX_JCC)
#define latxs_instptn_check_or_jcc_0() LATXS_INSTPTN_CHECK_XX_0(OR_JCC)
#define latxs_instptn_check_or_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(OR_XX_JCC)
#define latxs_instptn_check_xor_jcc_0() LATXS_INSTPTN_CHECK_XX_0(XOR_JCC)
#define latxs_instptn_check_xor_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(XOR_XX_JCC)
#define latxs_instptn_check_sar_jcc_0() LATXS_INSTPTN_CHECK_XX_0(SAR_JCC)
#define latxs_instptn_check_shr_jcc_0() LATXS_INSTPTN_CHECK_XX_0(SHR_JCC)
#define latxs_instptn_check_sub_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(SUB_XX_JCC)
#define latxs_instptn_check_shr_xx_jcc_0() LATXS_INSTPTN_CHECK_XX_0(SHR_XX_JCC)
#define latxs_instptn_check_dec_jcc_0() LATXS_INSTPTN_CHECK_XX_0(DEC_JCC)
#else
#define latxs_instptn_check_void(option)
#define latxs_instptn_check_false(option)
#define latxs_instptn_check_cmp_jcc_0()
#define latxs_instptn_check_test_jcc_0()
#define latxs_instptn_check_bt_jcc_0()
#define latxs_instptn_check_cqo_idiv_0()
#define latxs_instptn_check_cmp_sbb_0()
#define latxs_instptn_check_comisd_jcc_0()
#define latxs_instptn_check_comiss_jcc_0()
#define latxs_instptn_check_ucomisd_jcc_0()
#define latxs_instptn_check_ucomiss_jcc_0()
#define latxs_instptn_check_xor_div_0()
#define latxs_instptn_check_cdq_idiv_0()

#define latxs_instptn_check_cmp_xx_jcc_0()
#define latxs_instptn_check_test_xx_jcc_0()
#define latxs_instptn_check_bt_xx_jcc_0()
#define latxs_instptn_check_comisd_xx_jcc_0()
#define latxs_instptn_check_comiss_xx_jcc_0()
#define latxs_instptn_check_ucomisd_xx_jcc_0()
#define latxs_instptn_check_ucomiss_xx_jcc_0()

#define latxs_instptn_check_cmp_xxcc_con_0()
#define latxs_instptn_check_test_xxcc_con_0()

#define latxs_instptn_check_ucomisd_seta_0()

#define latxs_instptn_check_add_jcc_0()
#define latxs_instptn_check_sub_jcc_0()
#define latxs_instptn_check_and_jcc_0()
#define latxs_instptn_check_and_xx_jcc_0()
#define latxs_instptn_check_or_jcc_0()
#define latxs_instptn_check_or_xx_jcc_0()
#define latxs_instptn_check_xor_jcc_0()
#define latxs_instptn_check_xor_xx_jcc_0()
#define latxs_instptn_check_sar_jcc_0()
#define latxs_instptn_check_shr_jcc_0()
#define latxs_instptn_check_sub_xx_jcc_0()
#define latxs_instptn_check_shr_xx_jcc_0()
#define latxs_instptn_check_dec_jcc_0()
#endif

#ifdef LATXS_INSTPTN_CPL_FILTER
#define latxs_instptn_cpl_filter_void(tb) do {      \
    uint32_t cpl = (tb)->flags & HF_CPL_MASK;       \
    if (cpl != 3 && cpl != 0) return;               \
} while (0)
#else
#define latxs_instptn_cpl_filter_void(tb)
#endif

#else /* no LATXS_INSTPTN_ENABLE */

#define latxs_inst_pattern(tb)
#endif /* LATXS_INSTPTN_ENABLE */

#endif
