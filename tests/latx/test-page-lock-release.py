#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Regression test for the page-lock release safety net.

The helper extracts the production registry/page_unlock/tb_release code and
runs it against a synthetic PageDesc table.  It covers the crash where
dune_loop() is entered by a HAMT helper thread that has no TCG context.

Usage: python3 tests/latx/test-page-lock-release.py [source-root]
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
maintenance = (root / "accel/tcg/tb-maint.c").read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


registry = maintenance[
    maintenance.index("#define LATX_PAGE_LOCK_REGISTRY_MAX"):
    maintenance.index("static void page_lock(PageDesc *pd)")
]
page_unlock = function(maintenance, "static void page_unlock(PageDesc *pd)")
release = function(maintenance, "bool tb_release_ctx_page_locks(void)")

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_PAGE_BITS 12
typedef uint64_t tb_page_addr_t;

typedef struct QemuSpin {
    int value;
} QemuSpin;

typedef struct PageDesc {
    QemuSpin lock;
    int lock_owner;
} PageDesc;

struct TCGContext {
    tb_page_addr_t pre_locked_page0;
    tb_page_addr_t pre_locked_page1;
};

struct TranslationData {
    int is_tu_mode;
};

struct LatxEnv {
    struct TranslationData *tr_data;
};

static int current_tid = 1;
static int cp_tu;
static int tu_unlock_count;
static struct TCGContext ctx;
static struct TCGContext *tcg_ctx;
static struct TranslationData tr_data;
static struct LatxEnv env;
static struct LatxEnv *lsenv;

#define NR_PAGES 8
static PageDesc pages[NR_PAGES];

static int qemu_get_thread_id(void)
{
    return current_tid;
}

static void qemu_spin_unlock(QemuSpin *spin)
{
    spin->value = 0;
}

static void page_unlock__debug(const PageDesc *pd)
{
    (void)pd;
}

static int cp_tu_pages(void)
{
    return cp_tu;
}

static void tb_unlock_tu_pages(void)
{
    tu_unlock_count++;
}

static PageDesc *page_find_alloc(tb_page_addr_t index, bool alloc)
{
    (void)alloc;
    if (index >= NR_PAGES) {
        return NULL;
    }
    return &pages[index];
}

__REGISTRY__
__PAGE_UNLOCK__
__RELEASE__

static void reset_fixture(void)
{
    memset(pages, 0, sizeof(pages));
    for (int i = 0; i < NR_PAGES; i++) {
        pages[i].lock_owner = -1;
    }
    latx_page_lock_registry_num = 0;
    latx_page_lock_registry_overflow = false;
    current_tid = 1;
    cp_tu = 0;
    tu_unlock_count = 0;
    ctx.pre_locked_page0 = (tb_page_addr_t)-1;
    ctx.pre_locked_page1 = (tb_page_addr_t)-1;
    tcg_ctx = &ctx;
    tr_data.is_tu_mode = 0;
    env.tr_data = &tr_data;
    lsenv = &env;
}

static void test_null_tcg_context_with_registry(void)
{
    reset_fixture();
    tcg_ctx = NULL;
    pages[5].lock.value = 1;
    pages[5].lock_owner = current_tid;
    page_lock_registry_add(&pages[5]);

    /* Must not dereference tcg_ctx, but must still release this thread's lock. */
    assert(tb_release_ctx_page_locks());
    assert(pages[5].lock.value == 0);
    assert(pages[5].lock_owner == -1);
    assert(latx_page_lock_registry_num == 0);
}

static void test_null_tcg_context_empty_registry(void)
{
    reset_fixture();
    tcg_ctx = NULL;
    assert(!tb_release_ctx_page_locks());
}

static void test_pre_locked_page_release(void)
{
    reset_fixture();
    ctx.pre_locked_page0 = 1 << TARGET_PAGE_BITS;
    pages[1].lock.value = 1;
    pages[1].lock_owner = current_tid;
    page_lock_registry_add(&pages[1]);

    assert(tb_release_ctx_page_locks());
    assert(ctx.pre_locked_page0 == (tb_page_addr_t)-1);
    assert(pages[1].lock.value == 0);
    assert(pages[1].lock_owner == -1);
    assert(latx_page_lock_registry_num == 0);
}

static void test_foreign_owner_not_released(void)
{
    reset_fixture();
    ctx.pre_locked_page0 = 2 << TARGET_PAGE_BITS;
    pages[2].lock.value = 1;
    pages[2].lock_owner = 99;
    page_lock_registry_add(&pages[2]);

    assert(!tb_release_ctx_page_locks());
    assert(ctx.pre_locked_page0 == (tb_page_addr_t)-1);
    assert(pages[2].lock.value == 1);
    assert(pages[2].lock_owner == 99);
    /* Stale registry entries are dropped without touching the lock. */
    assert(latx_page_lock_registry_num == 0);
}

static void test_null_lsenv(void)
{
    reset_fixture();
    lsenv = NULL;
    assert(!tb_release_ctx_page_locks());
}

static void test_tu_unlock_is_still_called(void)
{
    reset_fixture();
    cp_tu = 1;
    tr_data.is_tu_mode = 1;
    assert(!tb_release_ctx_page_locks());
    assert(tu_unlock_count == 1);
}

int main(void)
{
    test_null_tcg_context_with_registry();
    test_null_tcg_context_empty_registry();
    test_pre_locked_page_release();
    test_foreign_owner_not_released();
    test_null_lsenv();
    test_tu_unlock_is_still_called();
    puts("PASS: page-lock release handles NULL TCG context and owner checks");
    return 0;
}
'''

fixture = fixture.replace("__REGISTRY__", registry)
fixture = fixture.replace("__PAGE_UNLOCK__", page_unlock)
fixture = fixture.replace("__RELEASE__", release)

with tempfile.TemporaryDirectory(prefix="latx-page-lock-release-") as temp:
    source = Path(temp) / "test.c"
    executable = Path(temp) / "test"
    source.write_text(fixture)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(
        compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(executable)],
        check=True, timeout=60,
    )
    subprocess.run([str(executable)], check=True, timeout=15)
