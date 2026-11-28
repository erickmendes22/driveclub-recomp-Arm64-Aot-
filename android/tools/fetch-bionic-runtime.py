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
import re
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
PLATFORM_ROOTS = ("libandroid.so", "liblog.so", "libaaudio.so", "libz.so")
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


def verified_output(path: Path, platform_libraries: bool = False) -> bool:
    try:
        metadata = json.loads((path / "bionic-runtime.json").read_text())
        expected = set(FILES)
        if platform_libraries:
            expected.update("system/lib64/" + name for name in PLATFORM_ROOTS)
        actual = set(metadata["files"])
        return (metadata["source_sha256"] == SOURCE_SHA256
                and metadata.get("platform_libraries", False) == platform_libraries
                and (expected <= actual if platform_libraries else expected == actual)
                and all(Path(name).parts[:2] == ("system", "lib64") or name == "system/bin/linker64"
                        for name in actual)
                and all((path / name).resolve().is_relative_to(path.resolve()) for name in actual)
                and all(digest(path / name) == metadata["files"][name]
                        for name in actual))
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


def elf_needed(path: Path) -> list[str]:
    """Read DT_NEEDED from actual image libraries; no host readelf is needed."""
    data = path.read_bytes()
    require(len(data) >= 64 and data[:7] == b"\x7fELF\x02\x01\x01"
            and struct.unpack_from("<H", data, 18)[0] == 183, f"Not AArch64 ELF: {path.name}")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    entsize, count = struct.unpack_from("<HH", data, 54)
    require(entsize == 56 and 0 < count < 4096 and phoff + count * entsize <= len(data),
            f"Invalid ELF program headers: {path.name}")
    segments = [struct.unpack_from("<IIQQQQQQ", data, phoff + index * entsize) for index in range(count)]
    dynamic = [entry for entry in segments if entry[0] == 2]
    require(len(dynamic) == 1, f"Invalid ELF dynamic section: {path.name}")
    offset, size = dynamic[0][2], dynamic[0][5]
    require(offset + size <= len(data) and size % 16 == 0, "Truncated dynamic section")
    tags: dict[int, list[int]] = {}
    for pos in range(offset, offset + size, 16):
        tag, value = struct.unpack_from("<qQ", data, pos)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    if 1 not in tags:
        return []
    require(5 in tags and 10 in tags, "Missing ELF dynamic strings")
    address, length = tags[5][0], tags[10][0]
    strings = None
    for kind, _, off, base, _, filesz, _, _ in segments:
        if kind == 1 and base <= address and address - base + length <= filesz:
            require(off + address - base + length <= len(data), "Truncated dynamic strings")
            strings = data[off + address - base:off + address - base + length]
            break
    require(strings is not None, "Unmapped dynamic strings")
    result = []
    for index in tags[1]:
        require(index < len(strings), "Invalid DT_NEEDED string offset")
        end = strings.find(b"\0", index)
        require(end >= 0, "Unterminated DT_NEEDED string")
        name = strings[index:end].decode("ascii")
        require(re.fullmatch(r"[A-Za-z0-9_+@.\-]+\.so", name) is not None,
                f"Unsupported Android dependency name: {name!r}")
        result.append(name)
    return result


def extract_platform(debugfs: str, system: Path, temp: Path, result: Path) -> dict[str, str]:
    """Resolve the NDK platform libraries from the same pinned Android image.

    The real transitive dependencies are included, including runtime/ART/media
    APEX files when necessary. These are test-only system files, never APK
    substitutes or generated success stubs.
    """
    listing = subprocess.run([debugfs, "-R", "ls -p /system/apex", str(system)],
                             text=True, capture_output=True, check=True)
    apex_names = sorted({parts[5] for line in listing.stdout.splitlines()
                         if len(parts := line.split("/")) > 6
                         and re.fullmatch(r"[A-Za-z0-9_.\-]+\.apex", parts[5])})
    apex_images: dict[str, Path] = {}

    def try_library(image: Path, internal: str, target: Path) -> bool:
        try:
            dump(debugfs, image, internal, target)
            elf_needed(target)
            return True
        except RuntimeError:
            target.unlink(missing_ok=True)
            return False

    def apex_image(name: str) -> Path:
        if name not in apex_images:
            archive, payload = temp / name, temp / (name + ".ext4")
            dump(debugfs, system, "/system/apex/" + name, archive)
            with zipfile.ZipFile(archive) as package, package.open("apex_payload.img") as source, payload.open("wb") as target:
                shutil.copyfileobj(source, target)
            archive.unlink()
            apex_images[name] = payload
        return apex_images[name]

    pending = list(PLATFORM_ROOTS)
    sources: dict[str, str] = {}
    visited = set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        require(len(visited) <= 512, "Unexpected platform dependency closure size")
        relative = "system/lib64/" + name
        target = result / relative
        if not target.is_file():
            if try_library(system, "/system/lib64/" + name, target):
                sources[relative] = "/system/lib64/" + name
            else:
                for apex_name in apex_names:
                    if try_library(apex_image(apex_name), "/lib64/" + name, target):
                        sources[relative] = "/system/apex/" + apex_name + ":/lib64/" + name
                        break
                else:
                    raise RuntimeError(f"The pinned Android image is missing platform dependency {name}")
        pending.extend(elf_needed(target))
    print(f"Extracted actual Android platform dependency closure: {len(visited)} libraries", flush=True)
    return sources


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cache-dir", type=Path, help="Retain the verified Google archive for future runs")
    parser.add_argument("--platform-libraries", action="store_true",
                        help="Also extract real NDK platform dependencies for a linked APK resolver test")
    parser.add_argument("--debugfs", default=shutil.which("debugfs"), help="Path to debugfs from e2fsprogs")
    args = parser.parse_args()
    output = args.output.resolve()
    if verified_output(output, args.platform_libraries):
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
        payload = temp / "runtime.ext4"
        with zipfile.ZipFile(apex) as package, package.open("apex_payload.img") as source, payload.open("wb") as target:
            shutil.copyfileobj(source, target)
        files = {}
        for relative, internal in FILES.items():
            target = temp / "result" / relative
            dump(debugfs, payload, internal, target)
            require(target.read_bytes()[:6] == b"\x7fELF\x02\x01", f"Not an ELF64 runtime file: {relative}")
            files[relative] = digest(target)
        platform_sources = {}
        if args.platform_libraries:
            platform_sources = extract_platform(debugfs, system, temp, temp / "result")
            for target in sorted((temp / "result/system/lib64").glob("*.so")):
                files[str(target.relative_to(temp / "result"))] = digest(target)
        (temp / "result/system/bin/linker64").chmod(0o755)
        metadata = {
            "android_api": 30, "abi": "arm64-v8a", "source_url": SOURCE_URL,
            "source_sha256": SOURCE_SHA256, "google_repository_sha1": SOURCE_SHA1,
            "purpose": "CPU/import test runtime only; never packaged in the APK", "files": files,
            "platform_libraries": args.platform_libraries, "platform_sources": platform_sources,
        }
        output.mkdir(parents=True, exist_ok=True)
        for relative in files:
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(temp / "result" / relative, destination)
        (output / "bionic-runtime.json").write_text(json.dumps(metadata, indent=2) + "\n")
    require(verified_output(output, args.platform_libraries), "Extracted runtime verification failed")
    print(f"Official API 30 ARM64 Bionic runtime ready: {output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"Bionic runtime extraction failed: {error}", file=sys.stderr)
        sys.exit(1)
