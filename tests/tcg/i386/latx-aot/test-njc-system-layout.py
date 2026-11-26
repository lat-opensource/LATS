#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check real allocator, C invalidation and emitted NJC address arithmetic."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
hash_source = (root / 'accel/tcg/tb-hash.h').read_text()
alloc_source = (root / 'accel/tcg/cpu-exec.c').read_text()
emit_source = (root / 'target/i386/latx/optimization/sys-intb-njc.c').read_text()
types = (root / 'accel/tcg/tb-jmp-cache.h').read_text()


def block(text, prefix):
    start = text.index(prefix)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


c = r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_SOFTMMU 1
#define TARGET_PAGE_BITS 12
#define TB_JMP_CACHE_BITS 12
#define TB_JMP_PAGE_BITS 6
#define TB_JMP_CACHE_SIZE 4096
#define TB_JMP_PAGE_SIZE 64
#define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
#define TB_JMP_PAGE_MASK (4096-64)
#define g_assert assert
#define g_assert_not_reached() abort()
#define G_STATIC_ASSERT(x) _Static_assert(x,#x)
typedef uint64_t vaddr;
struct rcu_head { void *next; void *function; };
'''
c += block(types, 'typedef struct LatxNJCEntry') + ' LatxNJCEntry;\n'
c += block(types, 'typedef struct LatxNJC {') + ' LatxNJC;\n'
c += r'''
typedef struct CPUJumpCache { struct rcu_head rcu; uintptr_t array[]; } CPUJumpCache;
typedef struct CPUState { CPUJumpCache *tb_jmp_cache; LatxNJC *latx_njc_cache; } CPUState;
typedef enum { LATX_JMP_CACHE_HASH_PAGE, LATX_JMP_CACHE_HASH_FULL, LATX_JMP_CACHE_HASH_LINE } LatxJmpCacheHashMode;
static unsigned bits=12;
static int option_table_addr;
static LatxJmpCacheHashMode hash_mode;
static unsigned latx_jmp_cache_bits(void) {return bits;}
static LatxJmpCacheHashMode latx_jmp_cache_hash_mode(void) {return hash_mode;}
static void *last_allocation;
static unsigned allocations, frees;
static void *qemu_memalign(size_t alignment,size_t size) {
    void *p=NULL;assert(!posix_memalign(&p,alignment,size));last_allocation=p;allocations++;return p;
}
static void qemu_vfree(void *p) {assert(p==last_allocation);frees++;free(p);last_allocation=NULL;}
'''
for prefix in ['static inline unsigned int tb_jmp_cache_hash_page_legacy(',
               'static inline unsigned int tb_jmp_cache_hash_legacy(',
               'static inline unsigned int latx_njc_hash_page(',
               'static inline LatxNJCEntry *latx_njc_entry(']:
    c += block(hash_source, prefix) + '\n'
c += 'static unsigned tb_jmp_cache_hash_page(vaddr pc) {return tb_jmp_cache_hash_page_legacy(pc);}\n'
c += block(hash_source,
           'static inline void tb_jmp_cache_flush_page_bank(') + '\n'
c += block(hash_source, 'static inline void tb_jmp_cache_flush_page_current(') + '\n'
c += block(alloc_source, 'static LatxNJC *latx_njc_aligned_alloc(') + '\n'
c += block(alloc_source, 'static void latx_njc_aligned_free(') + '\n'
c += r'''
typedef struct { unsigned val; } IR2_OPND;
static IR2_OPND latxs_env_ir2_opnd={23};
static void *lsenv;
static int lsenv_offset_of_latx_njc_cache_ptr(void *p) {(void)p;return 128;}
enum { LISA_SLLI_D, LISA_SRLI_D, LISA_XOR, LISA_BSTRPICK_D, LISA_BSTRINS_D, LISA_LD_D };
static uint64_t r[32], cache_base;
static unsigned emitted;
static uint64_t low_mask(unsigned n) {assert(n>0 && n<=64);return n==64?UINT64_MAX:(UINT64_C(1)<<n)-1;}
static void out2i(int op,IR2_OPND *d,IR2_OPND *s,unsigned imm) {
    emitted++;
    if(op==LISA_SRLI_D) {assert(imm<64);r[d->val]=r[s->val]>>imm;}
    else if(op==LISA_SLLI_D) {assert(imm<64);r[d->val]=r[s->val]<<imm;}
    else {assert(op==LISA_LD_D && s->val==23 && imm==128);r[d->val]=cache_base;}
}
static void out3(int op,IR2_OPND *d,IR2_OPND *a,IR2_OPND *b) {
    assert(op==LISA_XOR);emitted++;r[d->val]=r[a->val]^r[b->val];
}
static void out2ii(int op,IR2_OPND *d,IR2_OPND *s,unsigned hi,unsigned lo) {
    assert(lo<=hi && hi<64);emitted++;uint64_t mask=low_mask(hi-lo+1);
    if(op==LISA_BSTRPICK_D)r[d->val]=(r[s->val]>>lo)&mask;
    else {assert(op==LISA_BSTRINS_D);r[d->val]=(r[d->val]&~(mask<<lo))|((r[s->val]&mask)<<lo);}
}
#define la_out2i out2i
#define la_out3 out3
#define la_out2ii out2ii
'''
c += block(emit_source, 'void latxs_out_jmp_cache_hash(') + '\n'
c += block(emit_source, 'bool latxs_tr_gen_compact_njc_slot(') + '\n'
c += r'''
static unsigned logical_hash(uint64_t pc) {
    uint64_t mask=low_mask(bits);
    if(hash_mode==LATX_JMP_CACHE_HASH_FULL)return (pc^(pc>>bits))&mask;
    if(hash_mode==LATX_JMP_CACHE_HASH_LINE)return pc&mask;
    uint64_t tmp=pc^(pc>>6);return ((tmp>>6)&0xfc0)|(tmp&63);
}
static uint64_t random_state=UINT64_C(0x1234fedc98765432);
static uint64_t next_random(void) {
    random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;return random_state;
}
int main(void) {
    unsigned address_checks=0, bucket_checks=0;
    IR2_OPND pc={5},entry={8},tmp={9};
    /* Every combination of the PAGE hash bits must agree with C lookup. */
    hash_mode=LATX_JMP_CACHE_HASH_PAGE;
    for (option_table_addr=0; option_table_addr<=1; option_table_addr++) {
        for (unsigned v=0; v<(1u<<18); v++) {
            r[pc.val]=v; emitted=0;
            latxs_out_jmp_cache_hash(&pc, &tmp, &entry);
            assert(r[entry.val]==tb_jmp_cache_hash_legacy(v));
            assert(emitted==(option_table_addr ? 4u : 5u));
        }
    }
    for(hash_mode=0;hash_mode<=LATX_JMP_CACHE_HASH_LINE;hash_mode++)
    for(bits=8;bits<=20;bits++) {
        if(hash_mode==LATX_JMP_CACHE_HASH_PAGE && bits!=12)continue;
        size_t count=(size_t)1<<bits,bytes=count*sizeof(LatxNJCEntry);
        LatxNJC *cache=latx_njc_aligned_alloc(count);
        CPUState cpu={.latx_njc_cache=cache};
        assert(!((uintptr_t)cache->array&(bytes-1)));
        assert(!cache->array[0].tcptr && !cache->array[count-1].tcptr);
        cache->array[0].tcptr=(void *)1;cache->array[count-1].tcptr=(void *)2;
        for(unsigned enabled=0;enabled<2;enabled++)
        for(unsigned window=0;window<2;window++)
        for(unsigned i=0;i<2000;i++) {
            uint64_t value=i==0?0:i==1?UINT64_MAX:next_random();
            cache_base=(uintptr_t)cache->array | (window?UINT64_C(0x9000000000000000):0);
            r[pc.val]=value;r[entry.val]=UINT64_C(0xdeadbeef);r[tmp.val]=0;emitted=0;
            bool active=latxs_tr_gen_compact_njc_slot(&pc,&entry,&tmp,enabled);
            if(!enabled) {assert(!active && !emitted && r[entry.val]==UINT64_C(0xdeadbeef));continue;}
            unsigned h=logical_hash(value);
            uint64_t expected=(uintptr_t)latx_njc_entry(&cpu,h) | (window?UINT64_C(0x9000000000000000):0);
            assert(active && r[entry.val]==expected && r[pc.val]==value);
            unsigned old=hash_mode==LATX_JMP_CACHE_HASH_PAGE?8:hash_mode==LATX_JMP_CACHE_HASH_FULL?6:4;
            assert(emitted==old-2);
            address_checks++;
        }
        latx_njc_aligned_free(cache);assert(allocations==frees);
    }
    hash_mode=LATX_JMP_CACHE_HASH_PAGE;bits=12;
    CPUState cpu={.latx_njc_cache=latx_njc_aligned_alloc(4096)};
    cpu.tb_jmp_cache=calloc(1,sizeof(CPUJumpCache)+4096*sizeof(uintptr_t));assert(cpu.tb_jmp_cache);
    for(unsigned trial=0;trial<128;trial++) {
        uint64_t page=next_random()&~UINT64_C(4095);
        unsigned logical=tb_jmp_cache_hash_page(page),physical=latx_njc_hash_page(&cpu,page);
        assert(!(physical&63));
        for(unsigned offset=0;offset<4096;offset++) {
            assert((logical_hash(page+offset)&~63u)==physical);bucket_checks++;
        }
        for(unsigned h=0;h<4096;h++) {
            cpu.tb_jmp_cache->array[h]=h+1;
            latx_njc_entry(&cpu,h)->tcptr=(void *)(uintptr_t)(h+1);
        }
        tb_jmp_cache_flush_page_current(&cpu,page);
        for(unsigned h=0;h<4096;h++) {
            uintptr_t expected=(h&~63u)==logical?0:h+1;
            assert(cpu.tb_jmp_cache->array[h]==expected);
            assert((uintptr_t)latx_njc_entry(&cpu,h)->tcptr==expected);
        }
    }
    free(cpu.tb_jmp_cache);latx_njc_aligned_free(cpu.latx_njc_cache);assert(allocations==frees);
    printf("PASS: %u emitted-address cases, %u page-bucket checks, %u aligned allocation/free cycles; OFF unchanged; aligned address paths save two instructions\n",address_checks,bucket_checks,frees);
}
'''
with tempfile.TemporaryDirectory(prefix='njc-system-layout-') as directory:
    path = Path(directory)
    (path / 'layout.c').write_text(c)
    command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-O2',
               '-Wall', '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
               str(path / 'layout.c'), '-o', str(path / 'layout')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'layout')], check=True)
