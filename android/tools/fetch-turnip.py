#!/usr/bin/env python3
"""Fetch and verify exactly Turnip Gen8 V39-patched for the APK's assets.

The original ZIP is preserved as driver.zip. No variant substitution or
runtime download is performed. Add out/android-driver/assets to Gradle's
asset sources after running this script. --archive supports an offline copy
of the exact upstream ZIP; the same size/hash/metadata/ELF checks apply.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile
import urllib.request
import zipfile

PROJECT = Path(__file__).resolve().parents[2]
NOTICES = PROJECT / "android/third_party/turnip-gen8-v39-patched"
PIN = json.loads((NOTICES / "package.json").read_text())


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect_elf(data: bytes) -> dict:
    require(data[:7] == b"\x7fELF\x02\x01\x01", "Driver must be little-endian ELF64")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    kind, machine, _, _, phoff, shoff, _, _, phsize, phnum, shsize, shnum, _ = header
    require(kind == 3 and machine == 183, "Driver must be an AArch64 shared object")
    require(phsize == 56 and shsize == 64, "Unexpected ELF header layout")
    programs = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize) for i in range(phnum)]
    loads = [p for p in programs if p[0] == 1]
    require(loads and all(p[7] == PIN["elfLoadAlignment"] for p in loads), "Unexpected ELF load alignment")

    def file_offset(address: int) -> int:
        for p in loads:
            if p[3] <= address < p[3] + p[5]:
                return p[2] + address - p[3]
        raise ValueError("Dynamic address is outside ELF file mappings")

    dynamic = next(p for p in programs if p[0] == 2)
    tags: dict[int, list[int]] = {}
    for at in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<qQ", data, at)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    strings = data[file_offset(tags[5][0]):file_offset(tags[5][0]) + tags[10][0]]

    def string(table: bytes, at: int) -> str:
        end = table.index(b"\0", at)
        return table[at:end].decode("utf-8")

    needed = [string(strings, n) for n in tags.get(1, [])]
    require(needed == PIN["needed"], "Driver dependency list differs from pinned package")
    require(string(strings, tags[14][0]) == PIN["libraryName"], "Unexpected driver SONAME")
    api = None
    for p in programs:
        if p[0] != 4:
            continue
        at, end = p[2], p[2] + p[5]
        while at + 12 <= end:
            namesz, descsz, typ = struct.unpack_from("<III", data, at)
            at += 12
            owner = data[at:at + namesz].rstrip(b"\0")
            at += (namesz + 3) & ~3
            desc = data[at:at + descsz]
            at += (descsz + 3) & ~3
            if owner == b"Android" and typ == 1:
                api = struct.unpack_from("<I", desc)[0]
    require(api == PIN["androidBuildApi"], "Unexpected Android ABI note")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize) for i in range(shnum)]
    exports = set()
    for s in sections:
        if s[1] != 11:
            continue
        names = sections[s[6]]
        table = data[names[4]:names[4] + names[5]]
        for at in range(s[4], s[4] + s[5], s[9]):
            name, info, _, index, _, _ = struct.unpack_from("<IBBHQQ", data, at)
            if name and index and info >> 4 in (1, 2):
                exports.add(string(table, name))
    require("HMI" in exports, "Missing Android Vulkan HAL entry")
    require(b"Mesa 27.0.0-devel (git-2648b2059f)" in data, "Unexpected Mesa revision")
    return {"architecture": "AArch64", "androidAbiNote": api,
            "loadAlignment": min(p[7] for p in loads), "needed": needed, "halEntry": "HMI"}


def verify(archive: Path) -> tuple[bytes, dict]:
    require(archive.stat().st_size == PIN["assetSize"], "Incorrect ZIP size; expected the exact patched V39 asset")
    require(sha(archive.read_bytes()) == PIN["assetSha256"], "ZIP SHA-256 differs from the pinned patched V39 release")
    with zipfile.ZipFile(archive) as package:
        require(sorted(package.namelist()) == [PIN["libraryName"], "meta.json"], "Unexpected package entries")
        raw = package.read("meta.json")
        library = package.read(PIN["libraryName"])
    require(sha(raw) == PIN["metadataSha256"], "Package metadata checksum mismatch")
    metadata = json.loads(raw)
    require(metadata["name"] == PIN["displayName"] and metadata["libraryName"] == PIN["libraryName"],
            "Incorrect Turnip variant or library name")
    require(len(library) == PIN["librarySize"] and sha(library) == PIN["librarySha256"], "Driver ELF checksum mismatch")
    return raw, inspect_elf(library)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="Existing exact upstream ZIP, verified before use")
    parser.add_argument("--cache-dir", type=Path, default=PROJECT / "out/android-driver/source")
    parser.add_argument("--output", type=Path,
                        default=PROJECT / "out/android-driver/assets/drivers/turnip-gen8-v39-patched")
    args = parser.parse_args()
    archive = args.archive
    if archive is None:
        args.cache_dir.mkdir(parents=True, exist_ok=True)
        archive = args.cache_dir / PIN["assetName"]
        if not archive.exists():
            with tempfile.NamedTemporaryFile(dir=args.cache_dir, suffix=".download", delete=False) as temporary:
                downloading = Path(temporary.name)
            try:
                request = urllib.request.Request(PIN["assetUrl"], headers={"User-Agent": "Driveclub-build/1"})
                with urllib.request.urlopen(request, timeout=60) as response, downloading.open("wb") as output:
                    shutil.copyfileobj(response, output)
                verify(downloading)
                downloading.replace(archive)
            finally:
                downloading.unlink(missing_ok=True)
    raw, elf = verify(archive)
    args.output.mkdir(parents=True, exist_ok=True)
    files = {"driver.zip": archive.read_bytes(), "meta.json": raw}
    for source in NOTICES.iterdir():
        if source.is_file() and source.name != "package.json":
            files[source.name] = source.read_bytes()
    provenance = dict(PIN, bundledArchive="driver.zip", inspection=elf,
                      notices=sorted(n for n in files if n not in ("driver.zip", "meta.json")))
    files["provenance.json"] = (json.dumps(provenance, indent=2, ensure_ascii=False) + "\n").encode()
    for name, data in files.items():
        with tempfile.NamedTemporaryFile(dir=args.output, prefix=".staging-", delete=False) as temporary:
            temporary.write(data)
            staging = Path(temporary.name)
        staging.replace(args.output / name)
    print(f"Verified and bundled {PIN['displayName']}: {args.output}")
    print(f"ZIP SHA-256: {PIN['assetSha256']}")
    print(f"Driver SHA-256: {PIN['librarySha256']}")
    print("HAL driver requires AdrenoTools; actual device loading and Vulkan features still require validation.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, StopIteration, struct.error, zipfile.BadZipFile) as error:
        raise SystemExit(f"Turnip package validation failed: {error}")
