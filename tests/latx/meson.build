# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

if 'CONFIG_CAPSTONE_DIET' in config_host and 'CONFIG_LATX_DEBUG' not in config_host
  test_latx_ir1_decode = executable(
    'test-latx-ir1-decode',
    files('../unit/test-latx-ir1-decode.c') + genh,
    include_directories: include_directories('../..'),
    dependencies: [glib, qemuutil, capStone],
    build_by_default: false,
  )
  test('test-latx-ir1-decode', test_latx_ir1_decode,
       suite: ['unit', 'latx-decode'], timeout: 30)

  executable(
    'bench-latx-ir1-hot-fast',
    files('../unit/bench-latx-ir1-hot-fast.c') + genh,
    include_directories: include_directories('../..'),
    dependencies: [glib, qemuutil, capStone],
    build_by_default: false,
  )
endif

test_latx_scalar_compare_decode = executable(
  'test-latx-scalar-compare-decode',
  files('../unit/test-latx-scalar-compare-decode.c') + genh,
  include_directories: include_directories('../..'),
  dependencies: [glib, qemuutil, capStone],
  build_by_default: false,
)
test('test-latx-scalar-compare-decode', test_latx_scalar_compare_decode,
     suite: ['unit', 'latx-decode'], timeout: 30)

if targetos == 'linux' and 'CONFIG_LATX_OPT_BQL' in config_host
  test_qemu_cond_bql = executable(
    'test-qemu-cond-bql',
    files('../unit/test-qemu-cond-bql.c') + genh,
    dependencies: [glib, qemuutil],
    link_args: ['-Wl,--wrap=pthread_spin_unlock'],
    build_by_default: false,
  )
  test('test-qemu-cond-bql', test_qemu_cond_bql,
       suite: ['unit', 'latx-thread'], timeout: 15)
endif
