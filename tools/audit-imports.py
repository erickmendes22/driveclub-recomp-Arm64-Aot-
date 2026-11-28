#!/usr/bin/env python3
"""Locally audit supplied PS4 modules without preparing or executing a game image.

All module-derived reports, manifests and optional headers are private analysis
outputs. Keep them in ignored local storage, outside commits and CI artifacts.
Public resolver fixtures are generated separately by generate-provider-cases.py
from the port's source declarations, with no input from this module inventory.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


class AuditError(ValueError):
    pass


def span(data: bytes | bytearray, offset: int, size: int) -> bytes:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise AuditError(f"range {offset:#x}+{size:#x} exceeds input {len(data):#x}")
    return bytes(data[offset:offset + size])


def unpack(fmt: str, data: bytes | bytearray, offset: int = 0):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def encode_id(value: int) -> str:
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
    result = alphabet[value & 63]
    while value >> 6:
        value >>= 6
        result = alphabet[value & 63] + result
    return result


def parse_module(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] == b"\x7fELF":
        count, base = 0, 0
    elif data[:4] == b"O\x15=\x1d":
        count = unpack("<H", data, 24)[0]
        base = 32 + count * 32
    else:
        raise AuditError(f"{path.name}: expected decrypted PS4 SELF or ELF")
    if span(data, base, 7) != b"\x7fELF\x02\x01\x01" or unpack("<H", data, base + 18)[0] != 62:
        raise AuditError(f"{path.name}: expected x86-64 little-endian ELF")
    phoff = unpack("<Q", data, base + 32)[0]
    phsize, phcount = unpack("<HH", data, base + 54)
    if phsize != 56 or not 0 < phcount < 256:
        raise AuditError(f"{path.name}: unsupported program header layout")
    headers = [unpack("<IIQQQQQQ", data, base + phoff + 56 * i) for i in range(phcount)]
    end = max(p[2] + p[5] for p in headers)
    if end > 512 * 1024 * 1024:
        raise AuditError(f"{path.name}: ELF exceeds preparation's size limit")
    elf = bytearray(end)
    header_end = phoff + phcount * 56
    if header_end > end:
        raise AuditError(f"{path.name}: ELF headers exceed available segments")
    elf[:header_end] = span(data, base, header_end)
    covered = [(0, header_end)]
    if base == 0:
        for p in headers:
            elf[p[2]:p[2] + p[5]] = span(data, p[2], p[5])
            covered.append((p[2], p[2] + p[5]))
    for i in range(count):
        flags, offset, size, memory = unpack("<QQQQ", data, 32 + 32 * i)
        if not flags & 0x800:
            continue
        if flags & 10:
            raise AuditError(f"{path.name}: encrypted/compressed SELF segment")
        index = (flags >> 20) & 4095
        if index >= len(headers):
            raise AuditError(f"{path.name}: invalid SELF segment index")
        p = headers[index]
        if size != memory or size != p[5]:
            raise AuditError(f"{path.name}: unsupported blocked SELF segment")
        elf[p[2]:p[2] + size] = span(data, offset, size)
        covered.append((p[2], p[2] + size))
    for p in headers:
        if p[5] and p[0] in (1, 2, 0x61000000, 0x61000010):
            if not any(a <= p[2] and p[2] + p[5] <= z for a, z in covered):
                raise AuditError(f"{path.name}: required segment is missing")
    try:
        dynamic = next(p for p in headers if p[0] == 2)
        dynlib = next(p for p in headers if p[0] == 0x61000000)
    except StopIteration as exc:
        raise AuditError(f"{path.name}: dynamic/linker data missing") from exc
    tags, entries = {}, []
    for at in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = unpack("<QQ", elf, at)
        if not tag:
            break
        tags[tag] = value
        entries.append((tag, value))
    blob = span(elf, dynlib[2], dynlib[5])
    try:
        strings = span(blob, tags[0x61000035], tags[0x61000037])
        table = span(blob, tags[0x61000039], tags[0x6100003f])
    except KeyError as exc:
        raise AuditError(f"{path.name}: required linker tag missing") from exc

    def string(offset: int) -> str:
        if offset >= len(strings) or (end := strings.find(b"\0", offset)) < 0:
            raise AuditError(f"{path.name}: invalid symbol string")
        return strings[offset:end].decode("utf-8", errors="strict")

    libraries, modules = {}, {}
    for tag, value in entries:
        if tag in (0x61000013, 0x61000015, 0x6100000d, 0x6100000f):
            target = libraries if tag in (0x61000013, 0x61000015) else modules
            identity = (string(value & 0xffffffff), (value >> 32) & 65535)
            key = encode_id(value >> 48)
            if key in target and target[key] != identity:
                raise AuditError(f"{path.name}: ambiguous module/library identity")
            target[key] = identity
    symbols = []
    if len(table) % 24:
        raise AuditError(f"{path.name}: malformed symbol table")
    for at in range(0, len(table), 24):
        name, info, _, section, value, size = unpack("<IBBHQQ", table, at)
        symbols.append(dict(name=string(name), type=info & 15, binding=info >> 4,
                            section=section, value=value, size=size))
    relocs = []
    for start, size in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        reloc = span(blob, tags[start], tags[size])
        if len(reloc) % 24:
            raise AuditError(f"{path.name}: malformed relocation table")
        for at in range(0, len(reloc), 24):
            target, info, addend = unpack("<QQq", reloc, at)
            relocs.append((target, info & 0xffffffff, info >> 32, addend))
    segments = [(p[3], span(elf, p[2], p[5])) for p in headers if p[0] == 1 and p[1] & 1]
    return dict(filename=path.name, sha256=hashlib.sha256(data).hexdigest(), size=len(data),
                libraries=libraries, modules=modules, symbols=symbols, relocs=relocs,
                segments=segments, loads=[(p[3], p[6]) for p in headers if p[0] in (1, 0x61000010)])


def identity(module: dict, name: str):
    parts = name.split("#")
    if len(parts) != 3:
        return None
    try:
        lib, mod = module["libraries"][parts[1]], module["modules"][parts[2]]
    except KeyError as exc:
        raise AuditError(f"{module['filename']}: unknown identity in {name}") from exc
    return parts[0], lib[0], lib[1], mod[0], mod[1]


def collect_manifest(paths: list[Path], names: dict[str, str]) -> dict:
    modules = [parse_module(p) for p in paths]
    mains = [m for m in modules if m["filename"] == "eboot.bin"]
    if len(mains) != 1:
        raise AuditError("exactly one eboot.bin is required")
    if len({m["filename"] for m in modules}) != len(modules):
        raise AuditError("duplicate module filenames; choose the intended base/update versions")
    main = mains[0]
    lib_ids = {value: key for key, value in main["libraries"].items()}
    mod_ids = {value: key for key, value in main["modules"].items()}
    exports, libc_exports = {}, defaultdict(list)
    for m in modules:
        if m is main:
            continue  # Link() exports the linked modules, not eboot itself.
        for symbol in m["symbols"]:
            if not symbol["section"] or symbol["binding"] not in (1, 2) or symbol["type"] not in (1, 2):
                continue
            key = identity(m, symbol["name"])
            if not key:
                continue
            if not any(start <= symbol["value"] and symbol["value"] + max(1, symbol["size"]) <= start + size
                       for start, size in m["loads"]):
                raise AuditError(f"{m['filename']}: exported symbol is outside mapped load segments")
            if key in exports:
                raise AuditError(f"ambiguous native export {key}")
            provider = dict(module=m["filename"], offset=hex(symbol["value"]),
                            kind="data" if symbol["type"] == 1 else "function")
            exports[key] = provider
            if key[1] == "libc":
                libc_exports[key[0]].append(provider)
    grouped, weak_zero = {}, []
    for m in modules:
        imported = defaultdict(list)
        for target, kind, symbol_id, addend in m["relocs"]:
            if kind not in (1, 6, 7):
                continue
            if symbol_id >= len(m["symbols"]):
                raise AuditError(f"{m['filename']}: invalid relocation symbol")
            symbol = m["symbols"][symbol_id]
            if not symbol["section"]:
                imported[symbol_id].append(target)
        got_to_plt, calls = defaultdict(list), defaultdict(list)
        for base, code in m["segments"]:
            for match in re.finditer(b"\xff\x25", code):
                at = match.start()
                if at + 6 <= len(code):
                    target = base + at + 6 + unpack("<i", code, at + 2)[0]
                    got_to_plt[target].append(base + at)
            for match in re.finditer(b"[\xe8\xe9]", code):
                at = match.start()
                if at + 5 <= len(code):
                    target = base + at + 5 + unpack("<i", code, at + 1)[0]
                    calls[target].append(base + at)
        for symbol_id, relocations in imported.items():
            symbol = m["symbols"][symbol_id]
            key = identity(m, symbol["name"])
            if not key:
                if symbol["binding"] != 2:
                    raise AuditError(f"{m['filename']}: required import without identity: {symbol['name']}")
                weak_zero.append(dict(module=m["filename"], name=symbol["name"], relocations=len(relocations)))
                continue
            kind = "data" if symbol["type"] == 1 else "function"
            if key not in grouped:
                nid, lib, libver, mod, modver = key
                runtime_name = nid + "#" + lib
                if (lib, libver) in lib_ids and (mod, modver) in mod_ids:
                    runtime_name = nid + "#" + lib_ids[(lib, libver)] + "#" + mod_ids[(mod, modver)]
                provider = exports.get(key)
                if provider is None and lib == "libSceLibcInternal" and len(libc_exports[nid]) == 1:
                    provider = libc_exports[nid][0]
                grouped[key] = dict(nid=nid, runtime_name=runtime_name, symbol=names.get(nid), kind=kind,
                                    library=lib, library_version=libver, module=mod, module_version=modver,
                                    weak=True, native_provider=provider, references=[])
            item = grouped[key]
            if item["kind"] != kind:
                raise AuditError(f"conflicting import kinds for {key}")
            if item["native_provider"] and item["native_provider"]["kind"] != kind:
                raise AuditError(f"native export kind mismatch for {key}")
            item["weak"] &= symbol["binding"] == 2
            plts = sorted({p for r in relocations for p in got_to_plt[r]})
            item["references"].append(dict(module=m["filename"], original_name=symbol["name"],
                relocations=[hex(r) for r in relocations], plt=[hex(p) for p in plts],
                direct_call_candidates=[hex(c) for p in plts for c in calls[p]]))
    return dict(schema=1, source="Relocated imports from supplied Driveclub 1.28 ELF/SELF modules",
                modules=[{k: m[k] for k in ("filename", "sha256", "size")} for m in modules],
                imports=sorted(grouped.values(), key=lambda item: (item["library"], item["nid"], item["kind"])),
                weak_unscoped_zero=weak_zero)


def uncomment(text: str) -> str:
    # Keep newlines so provider evidence still points to source lines.
    pattern = r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda m: m.group(0) if m.group(1) else
                  "\n" * m.group(0).count("\n"), text)


def source_providers(root: Path, names: dict[str, str]) -> dict:
    providers = defaultdict(list)
    by_symbol = defaultdict(list)

    def evidence(path: Path, text: str, at: int, function: str, kind="function"):
        return dict(source=str(path.relative_to(root)), line=text[:at].count("\n") + 1,
                    function=function.strip(), kind=kind)

    for path in sorted((root / "src").glob("runtime*.c")):
        if path.name == "runtime_pad.c":
            continue  # Android compiles runtime_pad_android.c instead.
        text = uncomment(path.read_text(encoding="utf-8", errors="replace"))
        for table in re.finditer(r'RuntimeExport\s+\w+\s*\[\s*\]\s*=\s*\{([\s\S]*?)\};', text):
            for entry in re.finditer(r'\{\s*"([^"\n]+)"\s*,\s*([^}]+)\}', table.group(1)):
                by_symbol[entry[1]].append(evidence(path, text, table.start(1) + entry.start(), entry[2]))
        for match in re.finditer(r'nid_match\(\s*name\s*,\s*"([^"\n]+)"\s*\)\)\s*return\s*\(uintptr_t\)([^;]+);', text):
            kind = "data" if path.name == "runtime.c" and match[1].split("#")[0] in {
                "djxxOmW6-aw", "f7uOxY9mM1U", "ZT4ODD2Ts9o"} else "function"
            providers[match[1].split("#")[0]].append(evidence(path, text, match.start(), match[2], kind))
        if path.name == "runtime_thread.c":
            for entry in re.finditer(r'\{\s*"([A-Za-z0-9+-]{11}(?:#[^"\n]*)?)"\s*,\s*([A-Za-z_]\w*)\s*\}', text):
                providers[entry[1].split("#")[0]].append(evidence(path, text, entry.start(), entry[2]))
    for nid, symbol in names.items():
        providers[nid].extend(by_symbol[symbol])
    library_root = root / "gpu/shadps4/core/libraries"
    gpu_paths = [library_root / p for p in (
        "gnmdriver/gnmdriver.cpp", "videoout/video_out.cpp", "kernel/equeue.cpp",
        "avplayer/avplayer.cpp", "videodec/videodec.cpp")]
    gpu_paths += [root / "gpu/bbnet/png_dec.cpp", root / "gpu/bbnet/image_codecs_register.cpp"]
    # These are the RegisterLib namespaces called by RegisterNetworkSymbols in
    # bbnet_glue.cpp. Do not count an unregistered/new .cpp file as a provider.
    gpu_paths += [library_root / p for p in (
        "network/http.cpp", "network/http2.cpp", "network/net.cpp", "network/netctl.cpp",
        "network/rudp.cpp", "network/ssl.cpp", "network/ssl2.cpp",
        "np/np_auth.cpp", "np/np_common.cpp", "np/np_manager.cpp",
        "np/np_matching2/np_matching2.cpp", "np/np_partner.cpp", "np/np_party.cpp",
        "np/np_score/np_score.cpp", "np/np_signaling/np_signaling.cpp", "np/np_tus.cpp",
        "np/np_web_api/np_web_api.cpp", "np/np_web_api2/np_web_api2.cpp")]
    for path in gpu_paths:
        if not path.exists():
            raise AuditError(f"missing compiled GPU registry source: {path}")
        text = uncomment(path.read_text(encoding="utf-8", errors="replace"))
        for match in re.finditer(r'LIB_(FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"[^"]+"\s*,\s*\d+\s*,\s*"[^"]+"\s*,\s*([^);]+)\s*\)', text):
            providers[match[2]].append(evidence(path, text, match.start(), match[3],
                                               "data" if match[1] == "OBJ" else "function"))
    return providers


def audit(manifest: dict, providers: dict) -> dict:
    result = json.loads(json.dumps(manifest))
    for item in result["imports"]:
        item["source_providers"] = [p for p in providers.get(item["nid"], []) if p["kind"] == item["kind"]]
        item["resolution"] = ("host-source-match" if item["source_providers"] else
                              "native-binding" if item["native_provider"] else "missing")
    result["summary"] = dict(Counter(item["resolution"] for item in result["imports"]))
    result["summary"].update(modules=len(result["modules"]), imports=len(result["imports"]),
                              required_missing=sum(item["resolution"] == "missing" and not item["weak"] for item in result["imports"]))
    result["scope"] = "Source matches are diagnostic evidence. Test generated import cases against the actual runtime_resolve and GPU registry before release. Direct call candidates are byte-pattern xrefs, not a reachability proof."
    return result


def write_header(manifest: dict, path: Path):
    lines = ["/* PRIVATE local import audit: tools/audit-imports.py. Do not commit or publish. */",
             "#ifndef DRIVECLUB_IMPORT_CASES_H", "#define DRIVECLUB_IMPORT_CASES_H",
             "typedef struct { const char *name, *symbol; int is_data, native_binding, weak, has_hle_expected; } DriveclubImportCase;",
             "static const DriveclubImportCase driveclub_import_cases[] = {"]
    for item in manifest["imports"]:
        lines.append("    {%s, %s, %d, %d, %d, %d}," % (json.dumps(item["runtime_name"]),
            json.dumps(item["symbol"] or item["nid"]), item["kind"] == "data", bool(item["native_provider"]),
            item["weak"], bool(item.get("source_providers"))))
    lines += ["};", "#define DRIVECLUB_IMPORT_CASE_COUNT (sizeof(driveclub_import_cases)/sizeof(driveclub_import_cases[0]))", "#endif", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def metadata_only(manifest: dict) -> dict:
    result = json.loads(json.dumps(manifest))
    for item in result["imports"]:
        for reference in item["references"]:
            if "relocations" in reference:
                reference["relocation_count"] = len(reference.pop("relocations"))
            if "direct_call_candidates" in reference:
                reference["direct_call_candidate_count"] = len(reference.pop("direct_call_candidates"))
            reference.pop("plt", None)
    return result


def write_manifest(manifest: dict, path: Path):
    # One symbol per line keeps the metadata diff reviewable and reasonably small.
    prefix = {key: value for key, value in manifest.items() if key != "imports"}
    text = json.dumps(prefix, indent=2)
    text = text[:-2] + ',\n  "imports": [\n'
    text += ",\n".join("    " + json.dumps(item, separators=(",", ":")) for item in manifest["imports"])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + "\n  ]\n}\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--modules", nargs="+", type=Path, help="Original modules or directories containing them")
    source.add_argument("--manifest", type=Path, help="Previously generated metadata; game files are not needed")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--write-manifest", type=Path)
    parser.add_argument("--metadata-only", action="store_true", help="Omit relocation/call addresses from the saved manifest; retain counts and identities")
    parser.add_argument("--header", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-complete", action="store_true", help="Return failure for required imports with no source/native provider")
    parser.add_argument("--list-missing", action="store_true", help="Print missing symbols as well as the JSON report")
    args = parser.parse_args(argv)
    names = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', (args.root / "src/import_names.inc").read_text()))
    try:
        if args.modules:
            paths = []
            for entry in args.modules:
                paths += sorted(p for p in entry.rglob("*") if p.is_file() and (p.name == "eboot.bin" or p.suffix == ".prx")) if entry.is_dir() else [entry]
            manifest = collect_manifest(paths, names)
        else:
            manifest = json.loads(args.manifest.read_text())
            if manifest.get("schema") != 1:
                raise AuditError("unsupported inventory schema")
        if args.write_manifest:
            write_manifest(metadata_only(manifest) if args.metadata_only else manifest, args.write_manifest)
        report = audit(manifest, source_providers(args.root, names))
        if args.header:
            write_header(report, args.header)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report["summary"], sort_keys=True))
        for item in report["imports"]:
            if args.list_missing and item["resolution"] == "missing":
                owners = ",".join(ref["module"] for ref in item["references"])
                print(f"MISSING {item['nid']} {item['symbol'] or '?'} [{item['kind']}] {item['library']} ({owners})")
        return 1 if args.require_complete and report["summary"]["required_missing"] else 0
    except (AuditError, OSError, KeyError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"import audit: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
