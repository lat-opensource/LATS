/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_HAMT_H_
#define _LATX_SYS_HAMT_H_

#define HAMT_PTW_CODE_SIZE 2048

typedef struct {
    void *codebase;
    void *database;
    void *handler;
    int type;
    int verbose;
} hamt_trampoline_data_t;

#define HAMT_TRAMPOLINE_ERR 3

void latxs_gen_hamt_trampoline(hamt_trampoline_data_t *d);

typedef struct {
    void *codebase;
    void *database;
    void *database2;
    void *subcodebase;
    void *begin;
    void *end;
    int *size;
    int type;
    int verbose;
} hamt_excp_t;

#define HAMT_BREAK_CODE_TOPLV0      0x4
#define HAMT_BREAK_CODE_TOPLV3      0x5

#define HAMT_EXCP_ENTRY_SYSCALL     1
#define HAMT_EXCP_ENTRY_TLBREFILL   2
#define HAMT_EXCP_INTERRUPT_IPI     3
#define HAMT_EXCP_ENTRY_TLBLDSTMY   4
#define HAMT_EXCP_ENTRY_ERROR_TLB   5
#define HAMT_EXCP_ENTRY_ERROR_STOP  6
#define HAMT_EXCP_ENTRY_BREAK       7
#define HAMT_EXCP_ENTRY_FPE         8
#define HAMT_EXCP_ENTRY_ALE         9
#define HAMT_EXCP_ENTRY_FAKE_FORK   10
#define HAMT_EXCP_ENTRY_TI          11
#define HAMT_EXCP_ENTRY_INE         12

void latxs_gen_hamt_excp(hamt_excp_t *d);
void latxs_gen_hamt_exception(hamt_excp_t *d);
void latxs_gen_hamt_exception_error_stop(hamt_excp_t *d);
void latxs_gen_hamt_int(hamt_excp_t *d);

#endif
