#!/usr/bin/env python3
"""Extract a pinned, official API 30 ARM64 Bionic runtime for CPU tests.

No root, loop mount, Docker, or Android device is required. The only external
extraction tool is debugfs (the e2fsprogs package). These files are test tooling;
they are not shipped in the APK. Google's system image is about 548 MB and the
temporary GPT image is about 3.2 GB. Temporary extraction files are removed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile


SOURCE_URL = "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"
SOURCE_SHA256 = "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"
SOURCE_SHA1 = "e96298145a5e0bfd6da4816f51b06c520d8dba72"
SOURCE_SIZE = 548363615
FILES = {
    "system/bin/linker64": "/bin/linker64",
    "system/lib64/libc.so": "/lib64/bionic/libc.so",
    "system/lib64/libdl.so": "/lib64/bionic/libdl.so",
    "system/lib64/libdl_android.so": "/lib64/bionic/libdl_android.so",
    "system/lib64/libm.so": "/lib64/bionic/libm.so",
}
SECTOR = 512


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def digest(path: Path, algorithm: str = "sha256") -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def verified_output(path: Path) -> bool:
    try:
        metadata = json.loads((path / "bionic-runtime.json").read_text())
        return (metadata["source_sha256"] == SOURCE_SHA256
                and set(metadata["files"]) == set(FILES)
                and all(digest(path / name) == metadata["files"][name]
                        for name in FILES))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def archive_at(path: Path) -> Path:
    if path.is_file():
        require(path.stat().st_size == SOURCE_SIZE and digest(path) == SOURCE_SHA256,
                f"Cached archive has the wrong hash: {path}; remove it and retry")
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(".partial")
    print(f"Downloading official Android 30 ARM64 system image: {SOURCE_URL}", flush=True)
    try:
        value = hashlib.sha256()
        size = 0
        next_progress = 64 * 1024 * 1024
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as source, partial.open("wb") as target:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                target.write(block)
                value.update(block)
                size += len(block)
                if size >= next_progress:
                    print(f"Downloaded {size // (1024 * 1024)} MiB", flush=True)
                    next_progress += 64 * 1024 * 1024
        require(size == SOURCE_SIZE and value.hexdigest() == SOURCE_SHA256,
                "Downloaded Android image failed its pinned size/SHA-256 check")
        partial.replace(path)
    finally:
        partial.unlink(missing_ok=True)
    return path


def read_at(source, offset: int, length: int) -> bytes:
    require(offset >= 0 and 0 <= length <= 1024 * 1024, "Invalid metadata read")
    source.seek(offset)
    data = source.read(length)
    require(len(data) == length, "Truncated image metadata")
    return data


def system_extents(image: Path) -> list[tuple[int | None, int]]:
    """Read GPT and Android logical-partition metadata, without mounting them."""
    with image.open("rb") as source:
        gpt = read_at(source, SECTOR, SECTOR)
        require(gpt[:8] == b"EFI PART", "Expected Google's GPT system image")
        table_lba, count, entry_size = struct.unpack_from("<QII", gpt, 72)
        require(0 < count <= 128 and 128 <= entry_size <= 512, "Invalid GPT entries")
        table = read_at(source, table_lba * SECTOR, count * entry_size)
        super_offset = super_size = None
        for index in range(count):
            entry = table[index * entry_size:(index + 1) * entry_size]
            name = entry[56:128].decode("utf-16le").split("\0", 1)[0]
            if name == "super":
                first, last = struct.unpack_from("<QQ", entry, 32)
                super_offset, super_size = first * SECTOR, (last - first + 1) * SECTOR
                break
        require(super_offset is not None and super_size > 12288, "Missing GPT super partition")
        geometry = read_at(source, super_offset + 4096, 4096)
        magic, size = struct.unpack_from("<II", geometry)
        require(magic == 0x616C4467 and size == 52, "Unsupported LP geometry")
        geometry_hash = geometry[8:40]
        geometry_check = geometry[:8] + bytes(32) + geometry[40:size]
        require(hashlib.sha256(geometry_check).digest() == geometry_hash, "LP geometry checksum mismatch")
        metadata_max, slots, block_size = struct.unpack_from("<III", geometry, 40)
        require(metadata_max <= 1024 * 1024 and slots >= 1 and block_size == 4096,
                "Unsupported LP metadata geometry")
        metadata = read_at(source, super_offset + 3 * 4096, metadata_max)
        magic, major, minor, header_size = struct.unpack_from("<IHHI", metadata)
        require(magic == 0x414C5030 and major == 10 and header_size in (128, 256),
                f"Unsupported LP header ({major}.{minor})")
        header_check = metadata[:12] + bytes(32) + metadata[44:header_size]
        require(hashlib.sha256(header_check).digest() == metadata[12:44], "LP header checksum mismatch")
        table_size = struct.unpack_from("<I", metadata, 44)[0]
        require(header_size + table_size <= len(metadata), "LP tables exceed metadata")
        tables = metadata[header_size:header_size + table_size]
        require(hashlib.sha256(tables).digest() == metadata[48:80], "LP tables checksum mismatch")
        part_offset, part_count, part_size = struct.unpack_from("<III", metadata, 80)
        extent_offset, extent_count, extent_size = struct.unpack_from("<III", metadata, 92)
        require(part_size == 52 and extent_size == 24
                and part_offset + part_count * part_size <= len(tables)
                and extent_offset + extent_count * extent_size <= len(tables), "Invalid LP table descriptors")
        for index in range(part_count):
            entry = tables[part_offset + index * part_size:part_offset + (index + 1) * part_size]
            if entry[:36].split(b"\0", 1)[0] != b"system":
                continue
            _, first, count, _ = struct.unpack_from("<IIII", entry, 36)
            require(count > 0 and first + count <= extent_count, "Invalid system extents")
            result = []
            for extent in range(first, first + count):
                sectors, kind, target, device = struct.unpack_from("<QIQI", tables, extent_offset + extent * extent_size)
                require(kind in (0, 1) and device == 0, "Unsupported LP extent target")
                require(kind == 1 or (target + sectors) * SECTOR <= super_size, "LP extent exceeds super partition")
                result.append((None if kind == 1 else super_offset + target * SECTOR, sectors * SECTOR))
            return result
    raise RuntimeError("Missing system logical partition")


def extract_system(image: Path, target: Path) -> None:
    extents = system_extents(image)
    with image.open("rb") as source, target.open("wb") as output:
        for offset, size in extents:
            if offset is None:
                output.seek(size, os.SEEK_CUR)
                continue
            source.seek(offset)
            while size:
                block = source.read(min(size, 1024 * 1024))
                require(block, "Truncated system extent")
                output.write(block)
                size -= len(block)
        output.truncate()


def dump(debugfs: str, image: Path, source: str, target: Path) -> None:
    # The debugfs command uses only fixed internal paths and a fixed basename;
    # the output directory itself can contain spaces or command punctuation.
    target.parent.mkdir(parents=True, exist_ok=True)
    command = f'dump -p "{source}" "{target.name}"'
    result = subprocess.run([debugfs, "-R", command, str(image)], cwd=target.parent,
                            text=True, capture_output=True)
    require(result.returncode == 0 and target.is_file() and target.stat().st_size > 0,
            f"debugfs could not extract {source}: {result.stderr.strip()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cache-dir", type=Path, help="Retain the verified Google archive for future runs")
    parser.add_argument("--debugfs", default=shutil.which("debugfs"), help="Path to debugfs from e2fsprogs")
    args = parser.parse_args()
    output = args.output.resolve()
    if verified_output(output):
        print(f"Verified cached official API 30 Bionic runtime: {output}")
        return 0
    require(args.debugfs, "debugfs is required; install e2fsprogs or pass --debugfs /path/to/debugfs")
    debugfs = shutil.which(args.debugfs)
    require(debugfs, f"debugfs executable not found: {args.debugfs}")
    debugfs = str(Path(debugfs).resolve())
    output.parent.mkdir(parents=True, exist_ok=True)
    # Use the output filesystem, not /tmp: /tmp may be a small RAM-backed mount.
    with tempfile.TemporaryDirectory(prefix="driveclub-bionic-", dir=output.parent) as temp_name:
        temp = Path(temp_name)
        cache = args.cache_dir.resolve() if args.cache_dir else temp
        archive = archive_at(cache / SOURCE_URL.rsplit("/", 1)[1])
        disk = temp / "system-gpt.img"
        print("Extracting Android logical system partition and runtime APEX", flush=True)
        with zipfile.ZipFile(archive) as package, package.open("arm64-v8a/system.img") as source, disk.open("wb") as target:
            shutil.copyfileobj(source, target, length=1024 * 1024)
        system = temp / "system.ext4"
        extract_system(disk, system)
        disk.unlink()
        apex = temp / "runtime.apex"
        dump(debugfs, system, "/system/apex/com.android.runtime.apex", apex)
        system.unlink()
        payload = temp / "runtime.ext4"
        with zipfile.ZipFile(apex) as package, package.open("apex_payload.img") as source, payload.open("wb") as target:
            shutil.copyfileobj(source, target)
        files = {}
        for relative, internal in FILES.items():
            target = temp / "result" / relative
            dump(debugfs, payload, internal, target)
            require(target.read_bytes()[:6] == b"\x7fELF\x02\x01", f"Not an ELF64 runtime file: {relative}")
            files[relative] = digest(target)
        (temp / "result/system/bin/linker64").chmod(0o755)
        metadata = {
            "android_api": 30, "abi": "arm64-v8a", "source_url": SOURCE_URL,
            "source_sha256": SOURCE_SHA256, "google_repository_sha1": SOURCE_SHA1,
            "purpose": "CPU test runtime only; never packaged in the APK", "files": files,
        }
        output.mkdir(parents=True, exist_ok=True)
        for relative in FILES:
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(temp / "result" / relative, destination)
        (output / "bionic-runtime.json").write_text(json.dumps(metadata, indent=2) + "\n")
    require(verified_output(output), "Extracted runtime verification failed")
    print(f"Official API 30 ARM64 Bionic runtime ready: {output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"Bionic runtime extraction failed: {error}", file=sys.stderr)
        sys.exit(1)
