#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a PCID_WAIT + PCID_SMP + PCID_BENCH ROM; emit one JSON result."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time


def request(stream, command, arguments=None):
    message = {'execute': command, 'id': command}
    if arguments is not None:
        message['arguments'] = arguments
    stream.write(json.dumps(message).encode() + b'\n')
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError('QMP connection closed')
        reply = json.loads(line)
        if reply.get('id') == command:
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            return reply['return']


def read_stats(text):
    stats = []
    for line in text.splitlines():
        if line.startswith('PCID cpu='):
            row, seen = line.split(' pcids=')
            fields = dict(re.findall(r'(\w+)=([\d,]+)', row))
            inv = fields.pop('invpcid')
            stats.append({**{k: int(v) for k, v in fields.items()},
                          'invpcid': list(map(int, inv.split(','))),
                          'pcids': list(map(int, seen.split()))})
    return stats


def wait_for_marker(proc, marker, size, timeout):
    deadline = time.monotonic() + timeout
    while not marker.exists() or marker.stat().st_size < size:
        if proc.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f'ROM did not finish; status={proc.poll()}')
        time.sleep(0.01)


def check_reset(before, after, cache):
    if len(after) != len(before):
        raise RuntimeError('missing observations after reset')
    for first, second in zip(before, after):
        for key in ['cr3', 'enables', 'enabled', 'noflush']:
            if first[key] != second[key]:
                raise RuntimeError('counters were not reset: ' + key)
        if cache and first['cache_retained'] and not second['cache_retained']:
            raise RuntimeError('cache banks not reused after reset')


def run(binary, rom, mode, engine, timeout, cache=False, reset=False,
        la57=False, full_hash=False):
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        marker = work / 'result'
        sockpath = work / 'qmp'
        latx = ['select', 'aotsys=0', 'tumode=off', 'pcidstats=on',
                'pcid=off', 'pcidasid=off', 'pcidcache=off']
        if full_hash:
            latx.append('njchash=full')
        cpu = 'qemu64'
        if engine == 'lat':
            latx += ['hamt=1', 'hamtmode=1', 'hamtconfig=0x33e9']
        else:
            latx += ['hamt=0']
        if mode != 'A':
            cpu += ',+pcid,+invpcid'
            latx += ['pcid=on']
        if la57:
            cpu += ',+la57'
        if mode == 'C':
            latx += ['pcidasid=on']
        if cache:
            latx += ['pcidcache=on']
        args = [str(binary), '-machine', 'pc', '-accel', 'tcg,mode=' + engine,
                '-nodefaults',
                '-display', 'none', '-smp', '2', '-m', '32',
                '-bios', str(rom), '-debugcon', 'file:' + str(marker),
                '-qmp', f'unix:{sockpath},server=on,wait=off',
                '-cpu', cpu, '-latx', ','.join(latx)]
        if not reset:
            args.append('-no-reboot')
        expected = b'\x11' if mode == 'A' else b'\x10'
        proc = subprocess.Popen(args)
        try:
            wait_for_marker(proc, marker, 1, timeout)
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(10)
                sock.connect(str(sockpath))
                with sock.makefile('rwb', buffering=0) as stream:
                    request(stream, 'qmp_capabilities')
                    request(stream, 'stop')

                    def hmp(command):
                        return request(stream, 'human-monitor-command',
                                       {'command-line': command})

                    if marker.read_bytes() != expected:
                        raise RuntimeError('guest reported incorrect state:\n' +
                                           hmp('info registers -a'))
                    stats = read_stats(hmp('info jit'))
                    lines = hmp('xp /7wx 0x21100').splitlines()
                    words = [w for line in lines
                             for w in line.partition(':')[2].split()]
                    elapsed = int(words[0], 16) | int(words[1], 16) << 32
                    flags = int(words[2], 16)
                    smc = flags & 1
                    page_test = flags & 2
                    legacy_ticks = 0
                    if flags & 4:
                        legacy_ticks = (int(words[4], 16) |
                                        int(words[5], 16) << 32)
                        if not legacy_ticks or int(words[6], 16) != 1024:
                            raise RuntimeError('incomplete non-PCIDE CR3 loop')
                    probe_hits = int(words[3], 16) if flags & (2 | 8) else None
                    reset_stats = None
                    if reset:
                        request(stream, 'system_reset')
                        request(stream, 'cont')
                        wait_for_marker(proc, marker, 2, timeout)
                        request(stream, 'stop')
                        if marker.read_bytes() != expected * 2:
                            raise RuntimeError(
                                'incorrect state after reset:\n' +
                                hmp('info registers -a'))
                        reset_stats = read_stats(hmp('info jit'))
                        check_reset(stats, reset_stats, cache)
                    if cache and smc:
                        # The i440fx firmware interface selects and ejects AP 1.
                        # Its active and inactive banks both hold translations.
                        hmp('o/b 0xaf00 0')
                        hmp('o/w 0xaf00 1')
                        hmp('o/b 0xaf04 8')
                        cpus = request(stream, 'query-cpus-fast')
                        if [c['cpu-index'] for c in cpus] != [0]:
                            raise RuntimeError('AP remained present after eject')
            if len(stats) != 2:
                raise RuntimeError('missing vCPU observations')
            if mode != 'A' and (elapsed == 0 or not all(s['enables'] for s in stats)):
                raise RuntimeError('PCIDE was not enabled on both CPUs')
            if mode != 'A' and engine == 'lat' and not stats[0]['hw_refills']:
                raise RuntimeError('guest did not exercise HAMT hardware refill')
            if mode == 'C' and flags & 8:
                bsp = stats[0]
                if probe_hits * 10 < 4112 * 9:
                    raise RuntimeError(
                        f'INVLPG probe hits: {probe_hits}/4112')
                if (bsp['retained'] < 8192 or
                        (cache and bsp['cache_retained'] < 8192)):
                    raise RuntimeError('INVLPG discarded a PCID context')
                if bsp['invlpg_page'] < 4112 or not bsp['invlpg_large']:
                    raise RuntimeError('missing INVLPG invalidation path')
            if mode == 'C' and stats[0]['retained'] < 4096:
                raise RuntimeError('ASID retention not exercised')
            if cache and stats[0]['cache_retained'] < 4096:
                raise RuntimeError('software cache retention not exercised')
            if mode == 'C' and page_test:
                bsp = stats[0]
                if bsp.get('invpcid_page', 0) < 8192 or bsp['retained'] < 8192:
                    raise RuntimeError('4K INVPCID lost a PCID context')
                # Hardware can evict the probe independently of INVPCID.
                # Require it to stay hot on at least 90% of switch pairs.
                if probe_hits * 10 < 4112 * 9:
                    raise RuntimeError(
                        f'unrelated 4K page retained only {probe_hits} times')
                if bsp.get('invpcid_large', 0) < 2:
                    raise RuntimeError('large mappings did not widen INVPCID')
            if cache and smc and not stats[1]['cache_retained']:
                raise RuntimeError('inactive AP bank was not retained over SMC')
            return {'mode': mode, 'engine': engine, 'cache': cache,
                    'stats': stats, 'reset_stats': reset_stats, 'la57': la57,
                    'ap_unplugged': bool(cache and smc),
                    'unrelated_page_hits': probe_hits,
                    'switch_pair_tsc_ticks': elapsed,
                    'legacy_ticks': legacy_ticks,
                    'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('rom', type=Path)
    parser.add_argument('mode', choices='ABC')
    parser.add_argument('--engine', choices=['tcg', 'lat'], default='lat')
    parser.add_argument('--la57', action='store_true',
                        help='expose LA57 while the ROM uses four-level paging')
    parser.add_argument('--cache', action='store_true')
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--full-hash', action='store_true',
                        help='use the full-address jump-cache hash')
    parser.add_argument('--timeout', type=int, default=30)
    parser.add_argument('--output', type=Path)
    opts = parser.parse_args()
    if opts.engine == 'tcg' and opts.mode == 'C':
        parser.error('hardware ASID retention requires --engine lat')
    if opts.cache and opts.mode != 'C':
        parser.error('--cache requires C mode')
    result = run(opts.binary.resolve(), opts.rom.resolve(), opts.mode,
                 opts.engine, opts.timeout, opts.cache, opts.reset, opts.la57,
                 opts.full_hash)
    encoded = json.dumps(result, indent=2) + '\n'
    if opts.output:
        opts.output.write_text(encoded)
    else:
        print(encoded, end='')
