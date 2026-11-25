#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile and test storage/FDC default helpers extracted from production C files.

Usage: python3 tests/latx/test-storage-fdc-defaults.py [source-root]
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
blk_src = (root / "block/block-backend.c").read_text()
qcow2_src = (root / "block/qcow2.c").read_text()
vl_src = (root / "softmmu/vl.c").read_text()
piix_src = (root / "hw/i386/pc_piix.c").read_text()
ide_src = (root / "hw/ide/qdev.c").read_text()
machine_src = (root / "hw/core/machine.c").read_text()
block_src = (root / "block.c").read_text()
file_posix_src = (root / "block/file-posix.c").read_text()


def extract_fn(source: str, signature: str) -> str:
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end] + "\n"


def extract_defines(source: str, prefix: str) -> str:
    lines = [ln for ln in source.splitlines() if ln.startswith(prefix)]
    assert lines, f"missing defines starting with {prefix!r}"
    return "\n".join(lines) + "\n"


# Structural checks on machine/device property tables and file-posix open path
assert "m->no_floppy = 1;" in extract_fn(piix_src, "static void pc_i440fx_8_1_machine_options")
assert "m->no_floppy = 0;" in extract_fn(piix_src, "static void pc_i440fx_8_0_machine_options")
assert "!MACHINE_GET_CLASS(pcms)->no_floppy || qtest_enabled()" in piix_src
assert 'DEFINE_PROP_UINT16("rotation_rate", IDEDrive, dev.rotation_rate, 1),' in ide_src
assert '{ "ide-hd", "rotation_rate", "0" },' in machine_src
assert "*child_flags &= ~(BDRV_O_NATIVE_AIO | BDRV_O_AUTO_DIRECT);" in extract_fn(
    block_src, "static void bdrv_temp_snapshot_options"
)
assert "bdrv_flags & BDRV_O_AUTO_DIRECT" in file_posix_src
assert "!raw_is_ram_backed_fs(direct_fd)" in file_posix_src
assert "raw_probe_alignment(bs, direct_fd, &probe_err);" in file_posix_src
assert "aio_setup_linux_aio(bdrv_get_aio_context(bs), &laio_err)" in file_posix_src
assert "s->use_linux_io_uring = true;" not in file_posix_src.split("BDRV_O_AUTO_DIRECT", 1)[1]

pace_defs = extract_defines(blk_src, "#define BOOT_READ_PACE_")
fn_set_pace = extract_fn(blk_src, "void blk_set_boot_read_pace_enabled")
fn_reset_pace = extract_fn(blk_src, "void blk_reset_all_boot_read_pace")
fn_buffered = extract_fn(blk_src, "static bool GRAPH_RDLOCK bdrv_chain_has_buffered_protocol")
fn_calc_pace = extract_fn(blk_src, "static int64_t GRAPH_RDLOCK blk_calc_boot_read_pace_ns")
fn_accel_cb = extract_fn(vl_src, "static int accel_opt_is_qtest")
fn_vl_qtest = extract_fn(vl_src, "static bool vl_is_qtest_requested")

fixture = f"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MiB (1024ULL * 1024ULL)
#define GiB (1024ULL * MiB)
#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_REALTIME 0
#define BDRV_O_NOCACHE 0x0020
#define GRAPH_RDLOCK
#define GLOBAL_STATE_CODE() do {{}} while (0)
#define likely(x) (x)
#define unlikely(x) (x)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define g_str_equal(a, b) (strcmp((a), (b)) == 0)

typedef int QemuSpin;
static inline void qemu_spin_lock(QemuSpin *s) {{ (void)s; }}
static inline void qemu_spin_unlock(QemuSpin *s) {{ (void)s; }}

static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{{
    return (uint64_t)(((__uint128_t)a * b) / c);
}}

static int64_t fake_now_ns;
static inline int64_t qemu_clock_get_ns(int type)
{{
    (void)type;
    return fake_now_ns;
}}

typedef struct BlockDriverState BlockDriverState;
typedef struct BdrvChild {{
    BlockDriverState *bs;
}} BdrvChild;

struct BlockDriverState {{
    int open_flags;
    BdrvChild *file;
    BlockDriverState *cow_or_filter;
}};

static inline BlockDriverState *bdrv_filter_or_cow_bs(BlockDriverState *bs)
{{
    return bs->cow_or_filter;
}}

typedef struct BlockBackend BlockBackend;
struct BlockBackend {{
    QemuSpin boot_pace_lock;
    bool boot_pace_enabled;
    uint64_t boot_pace_rd_bytes;
    int64_t boot_pace_next_ns;
    struct {{
        struct {{
            void *throttle_state;
        }} throttle_group_member;
    }} public;
    BlockBackend *next;
}};

static struct {{
    BlockBackend *first;
}} block_backends;

#define QTAILQ_FOREACH(var, head, field) \\
    for ((var) = (head)->first; (var); (var) = (var)->next)

{pace_defs}
{fn_set_pace}
{fn_reset_pace}
{fn_buffered}
{fn_calc_pace}

/* Minimal stubs for vl_is_qtest_requested() */
typedef struct QemuOpts {{
    const char *accel;
}} QemuOpts;
typedef struct QDict {{
    const char *accel;
}} QDict;
typedef void Error;

static const char *qtest_chrdev;
static const char *accelerators;
static QDict *machine_opts_dict;
static const char *opt_list_accel;

static const char *qemu_opt_get(QemuOpts *opts, const char *name)
{{
    return strcmp(name, "accel") == 0 ? opts->accel : NULL;
}}

static const char *qdict_get_try_str(const QDict *d, const char *key)
{{
    return strcmp(key, "accel") == 0 ? d->accel : NULL;
}}

static void *qemu_find_opts(const char *group)
{{
    return (void *)group;
}}

static int qemu_opts_foreach(void *list,
                             int (*func)(void *, QemuOpts *, Error **),
                             void *opaque, Error **errp)
{{
    (void)list;
    if (opt_list_accel) {{
        QemuOpts o = {{ .accel = opt_list_accel }};
        return func(opaque, &o, errp);
    }}
    return 0;
}}

{fn_accel_cb}
{fn_vl_qtest}

int main(void)
{{
    /* 1. Direct chain (all protocol nodes have BDRV_O_NOCACHE): 0 sleep */
    BlockDriverState base_file = {{ .open_flags = BDRV_O_NOCACHE }};
    BdrvChild base_child = {{ .bs = &base_file }};
    BlockDriverState base_qcow2 = {{ .file = &base_child }};

    BlockDriverState ovl_file = {{ .open_flags = BDRV_O_NOCACHE }};
    BdrvChild ovl_child = {{ .bs = &ovl_file }};
    BlockDriverState ovl_qcow2 = {{
        .file = &ovl_child,
        .cow_or_filter = &base_qcow2,
    }};

    BlockBackend blk = {{ 0 }};
    block_backends.first = &blk;
    blk_set_boot_read_pace_enabled(&blk, true);
    fake_now_ns = 10 * NANOSECONDS_PER_SECOND;

    assert(!bdrv_chain_has_buffered_protocol(&ovl_qcow2));
    assert(blk_calc_boot_read_pace_ns(&blk, &ovl_qcow2, 64 * MiB) == 0);
    assert(blk.boot_pace_rd_bytes == 0);

    /* 2. Buffered backing file (e.g. cache=writeback or tmpfs fallback) */
    base_file.open_flags = 0;
    assert(bdrv_chain_has_buffered_protocol(&ovl_qcow2));

    /* Small bootmgr reads within ~4 MiB burst window do not sleep */
    for (int i = 0; i < 64; i++) {{
        assert(blk_calc_boot_read_pace_ns(&blk, &ovl_qcow2, 32 * 1024) == 0);
    }}
    assert(blk.boot_pace_rd_bytes == 2 * MiB);

    /* A large 36 MiB RAM-cached ReadyBoot chunk is paced to 180 MiB/s (~200 ms) */
    int64_t wait_ns = blk_calc_boot_read_pace_ns(&blk, &ovl_qcow2, 36 * MiB);
    assert(wait_ns > 150000000LL && wait_ns <= 210000000LL);

    /* Explicit user throttle_state bypasses automatic boot pacer */
    int dummy_throttle = 1;
    blk.public.throttle_group_member.throttle_state = &dummy_throttle;
    assert(blk_calc_boot_read_pace_ns(&blk, &ovl_qcow2, 36 * MiB) == 0);
    blk.public.throttle_group_member.throttle_state = NULL;

    /* Once cumulative boot reads reach 1 GiB, pacing disengages permanently */
    fake_now_ns += NANOSECONDS_PER_SECOND;
    (void)blk_calc_boot_read_pace_ns(&blk, &ovl_qcow2, 1000 * MiB);
    assert(blk.boot_pace_rd_bytes >= BOOT_READ_PACE_WINDOW_BYTES);
    assert(blk_calc_boot_read_pace_ns(&blk, &ovl_qcow2, 128 * MiB) == 0);

    /* Machine reset re-arms the 1 GiB window cleanly for guest reboots */
    blk_reset_all_boot_read_pace(NULL);
    assert(blk.boot_pace_rd_bytes == 0);
    assert(blk.boot_pace_next_ns == 0);

    /* 3. qtest detection across CLI forms */
    qtest_chrdev = NULL;
    accelerators = NULL;
    machine_opts_dict = NULL;
    opt_list_accel = NULL;
    assert(!vl_is_qtest_requested());

    qtest_chrdev = "unix:/tmp/qtest.sock";
    assert(vl_is_qtest_requested());
    qtest_chrdev = NULL;

    QDict mdict = {{ .accel = "qtest" }};
    machine_opts_dict = &mdict;
    assert(vl_is_qtest_requested());
    machine_opts_dict = NULL;

    opt_list_accel = "qtest";
    assert(vl_is_qtest_requested());
    opt_list_accel = NULL;

    printf("PASS: storage/FDC defaults, O_DIRECT chain detection, 1 GiB boot read pacer, and qtest guards\\n");
    return 0;
}}
"""

cc = shlex.split(os.environ.get("CC", "cc"))
with tempfile.TemporaryDirectory(prefix="lats-storage-fdc-") as tmp:
    c_path = Path(tmp) / "fixture.c"
    bin_path = Path(tmp) / "fixture"
    c_path.write_text(fixture)
    subprocess.run(
        [*cc, "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror", str(c_path), "-o", str(bin_path)],
        check=True,
    )
    subprocess.run([str(bin_path)], check=True)
