#!/usr/bin/env python3
"""Build and run the HAMT ptwflags guest behavior suite."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess

ROOT = Path(__file__).resolve().parents[5]
GUEST_DIR = "/home/ng/ptwflags-guest"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def qemu_command(args, init, snapshot, latx):
    command = [
        "setarch", platform.machine(), "-R", str(args.binary.resolve()),
        "-hda", str(args.disk.resolve()), "-m", "4096", "-smp", str(args.smp),
        "-kernel", str(args.kernel.resolve()), "-append",
        "console=ttyS0 root=/dev/sda1 rw panic=1 init=" + init,
        "-nographic", "-no-reboot", "-latx", latx,
    ]
    if snapshot:
        command.append("-snapshot")
    return command


def run_vm(args, name, init, snapshot, latx, expected_fault=False):
    log = args.output / (name + ".log")
    command = qemu_command(args, init, snapshot, latx)
    write_json(args.output / (name + ".command.json"), command)
    with log.open("wb") as stream:
        try:
            completed = subprocess.run(
                command, cwd=ROOT / "run", stdin=subprocess.DEVNULL,
                stdout=stream, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired:
            raise RuntimeError(f"{name} timed out; see {log}") from None
    text = log.read_text(errors="replace")
    if completed.returncode != 0:
        raise RuntimeError(f"{name} exited {completed.returncode}; see {log}")
    if expected_fault and "Kernel panic" not in text:
        raise RuntimeError(f"{name} did not report the expected init fault; see {log}")
    if not expected_fault and ("Kernel panic" in text or
                               "reboot: Power down" not in text):
        raise RuntimeError(f"{name} did not shut down cleanly; see {log}")
    return text, log


def check_build(text, log):
    required = (
        "PTWFLAGS_BUILD_RESULT=0",
        "PTWFLAGS_BUILD_USER=baseline",
        "PTWFLAGS_BUILD_USER=smc",
        "PTWFLAGS_BUILD_USER=vga",
        "PTWFLAGS_BUILD_USER=mmio",
        "PTWFLAGS_BUILD_USER=super_read",
        "PTWFLAGS_BUILD_USER=super_write",
        "PTWFLAGS_BUILD_USER=super_exec",
        "PTWFLAGS_BUILD_USER=super_write_cold",
        "PTWFLAGS_BUILD_USER=readonly_write",
        "PTWFLAGS_BUILD_USER=user_present_readonly_write",
    )
    missing = [marker for marker in required if marker not in text.splitlines()]
    if missing:
        raise RuntimeError(f"build markers missing {missing}; see {log}")


def check_core(name, text, log):
    marker = "PTWFLAGS_" + name.upper() + "=PASS"
    if marker not in text.splitlines():
        raise RuntimeError(f"missing {marker}; see {log}")


def check_fault(name, text, log):
    error = {
        "super_read": "5",
        "super_write": "7",
        "super_exec": "15",
        "super_write_cold": "7",
        "readonly_write": "6",
        "user_present_readonly_write": "7",
    }[name]
    pattern = (rf"ptwflags_user_[a-z]\[\d+\]: segfault at [^\n]+"
               rf"error {error}\b")
    if not re.search(pattern, text):
        raise RuntimeError(f"missing {name} fault with error {error}; see {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "disk", "kernel", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--timeout", default=300, type=int)
    parser.add_argument("--smp", default=1, type=int)
    parser.add_argument(
        "--build-latx",
        default="select,verbose=on,aotsys=0,tumode=off,tucpopt=off,hamt=0",
    )
    parser.add_argument(
        "--latx",
        default=("select,verbose=on,aotsys=0,tumode=off,tucpopt=off,"
                 "hamt=1,hamtmode=1,hamtsubmode=1,hamtconfig=0x3769,"
                 "pcidasid=off,pcidcache=off"),
    )
    parser.add_argument(
        "--only",
        choices=("baseline", "smc", "vga", "mmio",
                 "super_read", "super_write", "super_exec",
                 "super_write_cold", "readonly_write",
                 "user_present_readonly_write"),
    )
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("--output must not already exist")
    args.output.mkdir(parents=True)

    logs = {}
    if not args.skip_build:
        build_text, build_log = run_vm(
            args, "build", GUEST_DIR + "/build-in-guest.sh", False,
            args.build_latx,
        )
        check_build(build_text, build_log)
        logs["build"] = str(build_log)
    modes = ("baseline", "smc", "vga", "mmio")
    faults = ("super_read", "super_write", "super_exec", "super_write_cold",
              "readonly_write", "user_present_readonly_write")
    if args.only in modes:
        modes = (args.only,)
        faults = ()
    elif args.only in faults:
        modes = ()
        faults = (args.only,)

    for mode in modes:
        text, log = run_vm(
            args, mode, GUEST_DIR + "/run-" + mode.replace("_", "-") + ".sh",
            True, args.latx,
        )
        check_core(mode, text, log)
        logs[mode] = str(log)
    for mode in faults:
        text, log = run_vm(
            args, mode, GUEST_DIR + "/run-" + mode.replace("_", "-") + ".sh",
            True, args.latx, expected_fault=True,
        )
        check_fault(mode, text, log)
        logs[mode] = str(log)

    disk_stat = args.disk.stat()
    write_json(args.output / "result.json", {
        "result": "PASS",
        "binary_sha256": sha256(args.binary),
        "disk_size": disk_stat.st_size,
        "disk_mtime_ns": disk_stat.st_mtime_ns,
        "kernel_sha256": sha256(args.kernel),
        "latx": args.latx,
        "logs": logs,
    })
    print("PASS: ptwflags guest behavior suite; see", args.output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as exc:
        raise SystemExit(f"FAIL: {exc}")
