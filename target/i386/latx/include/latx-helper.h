/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_HELPER_H_
#define _LATX_HELPER_H_

const void *latx_helper_lookup_tb_hashtable(void *env);
const void *latx_helper_lookup_tb_hashtable_call(void *env);
const void *latx_helper_aot_resume(void *env);
const void *latx_helper_lookup_tb_call(void *env, void *source);
const void *latx_helper_aot_direct_call(void *env, void *source, uint64_t pc);
const void *latx_helper_aot_linked_call(void *env, void *source, uint64_t pc);
const void *latx_helper_aot_ordinary_call(void *env, void *source, uint64_t pc);
const void *latx_helper_aot_indirect_call(void *env, void *source, uint64_t pc,
                                        uint64_t *target_info);
uint32_t latx_helper_aot_direct_guard(void *env, void *source, uint64_t pc,
                                     uint32_t *slot);
const void *latx_helper_aot_ordinary_jump(void *env, uintptr_t exit);
void latx_helper_aot_cpu_query(CPUX86State *env, unsigned descriptor);
void latx_helper_aot_system(CPUX86State *env, unsigned descriptor, uint64_t operand);
void latx_helper_aot_x87(CPUX86State *env, unsigned descriptor, uint64_t operand);

double latx_helper_log2(double x);
double latx_helper_pow(double x, double y);
double latx_helper_atan2(double y, double x);
double latx_helper_cos(double x);
double latx_helper_sin(double x);

#endif
