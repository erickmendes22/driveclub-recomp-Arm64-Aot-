#!/usr/bin/env python3
"""Run an actual ARM64 Bionic test with the official runtime extracted by fetch-bionic-runtime.py.

On an ARM64 Linux host this invokes Android's linker directly. On x86-64 an
explicit --qemu enables emulation; that result is not a physical-device test.
The test directory must contain libbbcpu.so and libc++_shared.so extracted from
the APK under test. This runner does not build, replace, or patch those libraries.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def elf_arm64(path: Path) -> None:
    with path.open("rb") as source:
        header = source.read(64)
    require(header[:7] == b"\x7fELF\x02\x01\x01" and struct.unpack_from("<H", header, 18)[0] == 183,
            f"Expected little-endian AArch64 ELF: {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--qemu", type=Path, help="Optional qemu-aarch64 executable for an x86-64 host")
    parser.add_argument("test", type=Path)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    runtime, test = args.runtime.resolve(), args.test.resolve()
    metadata = json.loads((runtime / "bionic-runtime.json").read_text())
    require(metadata["android_api"] >= 30 and metadata["abi"] == "arm64-v8a", "API 30+ ARM64 runtime required")
    for relative, expected in metadata["files"].items():
        path = runtime / relative
        require(path.resolve().is_relative_to(runtime), "Invalid path in runtime manifest")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == expected, f"Runtime file changed: {relative}")
    linker = runtime / "system/bin/linker64"
    for binary in (linker, test, test.parent / "libbbcpu.so", test.parent / "libc++_shared.so"):
        elf_arm64(binary)
    library_path = str(test.parent) + ":" + str(runtime / "system/lib64")
    environment = os.environ.copy()
    environment.pop("LD_PRELOAD", None)
    # Passing the program's absolute path is required by Android's standalone linker.
    command = [str(linker), str(test), *args.arguments]
    if args.qemu:
        qemu = args.qemu.resolve()
        require(qemu.is_file(), f"QEMU executable not found: {qemu}")
        command = [str(qemu), "-cpu", "max", "-L", str(runtime), "-E", f"LD_LIBRARY_PATH={library_path}", *command]
        environment.pop("LD_LIBRARY_PATH", None)
        print("Executing ARM64 Bionic CPU test under QEMU; this does not validate device GPU or gameplay.", flush=True)
    else:
        require(platform.machine().lower() in ("aarch64", "arm64"),
                "A native ARM64 host is required; on x86-64 pass --qemu /path/to/qemu-aarch64-static")
        environment["LD_LIBRARY_PATH"] = library_path
        print("Executing ARM64 Bionic CPU test directly on the native ARM64 host.", flush=True)
    result = subprocess.run(command, env=environment)
    status = result.returncode if result.returncode >= 0 else 128 - result.returncode
    print(f"Bionic CPU test exit status: {status}", flush=True)
    return status


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, struct.error) as error:
        print(f"Bionic CPU test could not run: {error}", file=sys.stderr)
        sys.exit(1)
