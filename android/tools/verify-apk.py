#!/usr/bin/env python3
"""Audit the ARM64 APK's actual ELF payload, without host readelf dependencies.

This checks packaging and linking, not successful game execution. In particular,
16 KiB ELF alignment does not prove support for a 16 KiB Android kernel: the
renderer currently permits game execution only with 4 KiB host pages.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys
import zipfile


REQUIRED_EXPORTS = {
    "libdriveclub_android.so": {
        "Java_org_driveclub_port_PortActivity_probeGraphics",
        "Java_org_driveclub_port_PortActivity_inspect",
        "Java_org_driveclub_port_LocalGame_prepareNative",
        "Java_org_driveclub_port_DriverManager_configureNative",
    },
    "libdriveclub_game.so": {
        "Java_org_driveclub_port_GameActivity_runNative",
        "Java_org_driveclub_port_GameActivity_inputNative",
        "Java_org_driveclub_port_DriverManager_configureNative",
    },
    "libdriveclub_vulkan.so": {
        "driveclub_vk_configure", "driveclub_vk_is_configured",
        "driveclub_vk_get_instance_proc_addr", "driveclub_vk_probe",
        "driveclub_vk_description", "driveclub_vk_last_error",
        "driveclub_vk_generation", "driveclub_vk_lookup", "driveclub_vk_missing",
        "__wrap_vkCreateInstance", "__wrap_vkDestroyInstance", "__wrap_vkGetInstanceProcAddr",
    },
    "libadrenotools.so": {"adrenotools_open_libvulkan", "adrenotools_custom_driver_loaded"},
    "libhook_impl.so": {"init_hook_param", "hook_android_dlopen_ext", "hook_android_load_sphal_library"},
    "libmain_hook.so": {"android_dlopen_ext", "android_load_sphal_library"},
    "libdriveclub_pkg.so": {
        "Java_org_driveclub_port_PkgInstaller_inspectNative",
        "Java_org_driveclub_port_PkgInstaller_extractNative",
    },
    "libbbcpu.so": {
        "guest_cpu_init", "guest_cpu_code", "guest_cpu_code_changed",
        "guest_cpu_host_function", "guest_cpu_call", "guest_cpu_call_xmm",
        "guest_cpu_thread_stack", "guest_cpu_set_gs", "guest_cpu_abandon",
        "guest_cpu_thread_end", "guest_cpu_return_address", "guest_cpu_stack_pointer",
        "guest_cpu_signal_regs", "guest_cpu_host_pc", "guest_cpu_handle_fault",
        "guest_cpu_hook", "guest_cpu_tsc", "guest_cpu_tsc_frequency",
    },
    "libc++_shared.so": set(),
}
# Public NDK libraries provided by Android, not libraries expected in the APK.
ANDROID_SYSTEM_LIBRARIES = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
    "libvulkan.so", "libaaudio.so", "libz.so", "libmediandk.so",
    "libjnigraphics.so", "libOpenSLES.so", "libnativewindow.so",
}
DESKTOP_DEPENDENCY = re.compile(
    r"(?:lib(?:c|m|dl|pthread|rt|resolv|stdc\+\+|gcc_s)\.so\.[0-9]|"
    r"ld-linux|libSDL|libX11|libXext|libXrandr|libXfixes|libXcursor|libXinerama|"
    r"libXrender|libXi\.|libxcb|libxkbcommon)", re.IGNORECASE
)
TURNIP_ASSET = "assets/drivers/turnip-gen8-v39-patched/driver.zip"
TURNIP_ZIP_SHA256 = "bd457b30f195cf93a334b5302c221b1b5cb59c79c485935decf95d560a38c8a2"
TURNIP_LIBRARY = "libvulkan_freedreno.so"
TURNIP_LIBRARY_SHA256 = "2db3c4601bc3f046596f86d9d189cb5cad693812f8c8c55fe1817ec90ea879ac"
TURNIP_METADATA_SHA256 = "c40a758f839ecc340216ccd631221b57c00a4da5b7d6f2fb6ed60b67c19d1683"
# These are HAL namespace dependencies of the exact driver asset, not additional
# public NDK libraries allowed for the app's ordinary DT_NEEDED closure.
TURNIP_HAL_LIBRARIES = {
    "libhardware.so", "liblog.so", "libnativewindow.so", "libsync.so",
    "libm.so", "libz.so", "libdl.so", "libc.so",
}


class AuditError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditError(message)


def span(data: bytes, offset: int, size: int, label: str) -> bytes:
    require(offset >= 0 and size >= 0 and offset <= len(data) and size <= len(data) - offset,
            f"{label}: range outside ELF")
    return data[offset:offset + size]


def string_at(table: bytes, offset: int) -> str:
    require(0 <= offset < len(table), "ELF string offset outside table")
    end = table.find(b"\0", offset)
    require(end >= 0, "unterminated ELF string")
    return table[offset:end].decode("utf-8", "strict")


def inspect_elf(data: bytes, name: str, *, android_api_ceiling: int = 30,
                required_exports: set[str] | None = None) -> dict:
    require(len(data) >= 64 and data[:4] == b"\x7fELF", f"{name}: not an ELF file")
    require(data[4:7] == b"\x02\x01\x01", f"{name}: requires little-endian ELF64")
    (kind, machine, _, _, phoff, shoff, _, ehsize, phentsize, phnum,
     shentsize, shnum, _) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    require(kind == 3, f"{name}: expected ET_DYN shared library, got {kind}")
    require(machine == 183, f"{name}: expected AArch64 (183), got machine {machine}")
    require(ehsize == 64 and phentsize == 56, f"{name}: invalid ELF header sizes")
    require(0 < phnum < 65535, f"{name}: invalid program header count")
    span(data, phoff, phentsize * phnum, f"{name} program headers")
    programs = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize) for i in range(phnum)]
    loads = [entry for entry in programs if entry[0] == 1]
    require(loads, f"{name}: has no PT_LOAD segment")
    require(not any(entry[0] == 3 for entry in programs), f"{name}: shared library contains PT_INTERP")
    for _, _, offset, vaddr, _, filesz, memsz, align in loads:
        require(filesz <= memsz, f"{name}: PT_LOAD file size exceeds memory size")
        span(data, offset, filesz, f"{name} PT_LOAD")
        require(align >= 16384 and align & (align - 1) == 0,
                f"{name}: PT_LOAD alignment {align} is below 16 KiB or is invalid")
        require((vaddr - offset) % align == 0, f"{name}: incongruent PT_LOAD address/offset")

    def address_offset(address: int, length: int) -> int:
        for _, _, offset, vaddr, _, filesz, _, _ in loads:
            if vaddr <= address and address - vaddr <= filesz and length <= filesz - (address - vaddr):
                return offset + address - vaddr
        raise AuditError(f"{name}: dynamic address has no file-backed PT_LOAD")

    dynamic = [entry for entry in programs if entry[0] == 2]
    require(len(dynamic) == 1, f"{name}: expected one PT_DYNAMIC")
    dyn = span(data, dynamic[0][2], dynamic[0][5], f"{name} PT_DYNAMIC")
    tags: dict[int, list[int]] = {}
    terminated = False
    for pos in range(0, len(dyn) - 15, 16):
        tag, value = struct.unpack_from("<qQ", dyn, pos)
        if tag == 0:
            terminated = True
            break
        tags.setdefault(tag, []).append(value)
    require(terminated and 5 in tags and 10 in tags, f"{name}: incomplete dynamic section")
    strsz = tags[10][0]
    dynstr = span(data, address_offset(tags[5][0], strsz), strsz, f"{name} dynamic strings")
    needed = [string_at(dynstr, value) for value in tags.get(1, [])]
    require(len(needed) == len(set(needed)), f"{name}: duplicate DT_NEEDED entries")
    soname = string_at(dynstr, tags[14][0]) if 14 in tags else None
    require(soname == name, f"{name}: SONAME mismatch ({soname!r})")
    require(not any(token.startswith((b"GLIBC_", b"GLIBCXX_")) for token in dynstr.split(b"\0")),
            f"{name}: contains a GNU libc/libstdc++ version requirement")
    for dependency in needed:
        require("/" not in dependency and not DESKTOP_DEPENDENCY.search(dependency),
                f"{name}: desktop or absolute DT_NEEDED dependency {dependency!r}")

    android_api = None
    for entry in programs:
        if entry[0] != 4:
            continue
        notes = span(data, entry[2], entry[5], f"{name} PT_NOTE")
        pos = 0
        while pos + 12 <= len(notes):
            namesz, descsz, note_type = struct.unpack_from("<III", notes, pos)
            pos += 12
            owner = span(notes, pos, namesz, "ELF note owner").rstrip(b"\0")
            pos += (namesz + 3) & ~3
            desc = span(notes, pos, descsz, "ELF note descriptor")
            pos += (descsz + 3) & ~3
            if owner == b"Android" and note_type == 1 and descsz >= 4:
                android_api = struct.unpack_from("<I", desc)[0]
    require(android_api is not None, f"{name}: missing Android ABI identification note")
    require(21 <= android_api <= android_api_ceiling,
            f"{name}: Android ABI note requires API {android_api}, allowed ceiling is {android_api_ceiling}")

    require(shentsize == 64 and 0 < shnum < 65535, f"{name}: unavailable section table for symbol audit")
    span(data, shoff, shentsize * shnum, f"{name} section headers")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    exports: set[str] = set()
    imports: set[str] = set()
    dynsym_sections = [entry for entry in sections if entry[1] == 11]
    require(len(dynsym_sections) == 1, f"{name}: expected one dynamic symbol table")
    for section in dynsym_sections:
        _, _, _, _, offset, size, link, _, _, entsize = section
        require(entsize == 24 and size % 24 == 0 and link < len(sections), f"{name}: invalid dynamic symbols")
        strings = span(data, sections[link][4], sections[link][5], f"{name} symbol strings")
        symbols = span(data, offset, size, f"{name} dynamic symbols")
        for pos in range(0, size, 24):
            st_name, info, other, index, _, _ = struct.unpack_from("<IBBHQQ", symbols, pos)
            if not st_name:
                continue
            symbol = string_at(strings, st_name)
            binding, visibility = info >> 4, other & 3
            if index != 0 and binding in (1, 2) and visibility in (0, 3):
                exports.add(symbol)
            elif index == 0 and binding == 1:
                imports.add(symbol)
    missing = (REQUIRED_EXPORTS.get(name, set()) if required_exports is None else required_exports) - exports
    require(not missing, f"{name}: missing exports: {', '.join(sorted(missing))}")
    require("libc.so" in needed, f"{name}: missing Bionic libc.so dependency")
    return {
        "name": name, "machine": "AArch64", "android_api": android_api,
        "load_alignment": min(entry[7] for entry in loads), "needed": needed,
        "exports": exports, "strong_imports": imports,
    }


def audit_turnip(archive: zipfile.ZipFile) -> dict:
    require(TURNIP_ASSET in archive.namelist(), "APK is missing the bundled Turnip Gen8 V39-patched ZIP")
    package_data = archive.read(TURNIP_ASSET)
    require(len(package_data) == 2705084, "Bundled Turnip ZIP size differs from the exact patched V39 release")
    require(hashlib.sha256(package_data).hexdigest() == TURNIP_ZIP_SHA256,
            "Bundled Turnip ZIP SHA-256 differs from the exact patched V39 release")
    with zipfile.ZipFile(io.BytesIO(package_data)) as package:
        require(sorted(package.namelist()) == [TURNIP_LIBRARY, "meta.json"],
                "Bundled Turnip ZIP contains unexpected or duplicate entries")
        raw_metadata = package.read("meta.json")
        library = package.read(TURNIP_LIBRARY)
    require(hashlib.sha256(raw_metadata).hexdigest() == TURNIP_METADATA_SHA256,
            "Bundled Turnip metadata SHA-256 mismatch")
    metadata = json.loads(raw_metadata)
    require(metadata.get("schemaVersion") == 1 and metadata.get("name") == "Turnip Gen8 V39-patched"
            and metadata.get("libraryName") == TURNIP_LIBRARY and metadata.get("vendor") == "Mesa"
            and metadata.get("driverVersion") == "Vulkan 1.4.367" and metadata.get("minApi") == 28,
            "Bundled Turnip metadata identifies a different driver")
    require(len(library) == 8460728 and hashlib.sha256(library).hexdigest() == TURNIP_LIBRARY_SHA256,
            "Bundled Turnip ELF SHA-256/size mismatch")
    # The pinned external HAL was built with an API 34 note. It is an asset
    # loaded by AdrenoTools, not an app JNI library linked at the API 30 minimum.
    # Keep the ordinary JNI/library rule strict and check this exact asset separately.
    elf = inspect_elf(library, TURNIP_LIBRARY, android_api_ceiling=34, required_exports={"HMI"})
    require(elf["android_api"] == 34, "Bundled Turnip Android ABI note differs from the pinned asset")
    require(set(elf["needed"]) == TURNIP_HAL_LIBRARIES,
            "Bundled Turnip HAL dependencies differ from the pinned asset")
    require("vkGetInstanceProcAddr" not in elf["exports"],
            "Expected the pinned Android HAL driver, not a standalone Vulkan loader")
    outer_metadata = TURNIP_ASSET.rsplit("/", 1)[0] + "/meta.json"
    require(outer_metadata in archive.namelist() and archive.read(outer_metadata) == raw_metadata,
            "Bundled driver metadata does not match the exact ZIP")
    return {
        "name": metadata["name"], "asset": TURNIP_ASSET, "zip_sha256": TURNIP_ZIP_SHA256,
        "library": TURNIP_LIBRARY, "library_sha256": TURNIP_LIBRARY_SHA256,
        "metadata_min_api": metadata["minApi"], "android_build_api_note": elf["android_api"],
        "required_bionic_symbol_api": 30, "load_alignment": elf["load_alignment"],
        "hal_entry": "HMI", "needed": elf["needed"],
        "validation": "Exact bundled bytes and HAL ABI; device initialization is not proven by this audit",
    }


def audit(apk: Path) -> dict:
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)), "APK contains duplicate ZIP entries")
        libraries = [name for name in names if name.startswith("lib/") and name.endswith(".so")]
        require(all(name.startswith("lib/arm64-v8a/") for name in libraries),
                "Final APK contains an unexpected native ABI; expected arm64-v8a only")
        present = {name.rsplit("/", 1)[1]: name for name in libraries}
        missing = set(REQUIRED_EXPORTS) - set(present)
        require(not missing, f"APK missing required runtime libraries: {', '.join(sorted(missing))}")
        require(not any(re.search(r"(?:^|/)(?:eboot\.bin|boot-linked\.bin|game\d+\.dat)$", name, re.I) for name in names),
                "APK must prepare and open game data on-device; game executable/archive/prepared boot was packaged")
        results = {name: inspect_elf(archive.read(path), name) for name, path in sorted(present.items())}
        for name in ("libdriveclub_android.so", "libdriveclub_game.so", "libdriveclub_vulkan.so"):
            require("libvulkan.so" not in results[name]["needed"],
                    f"{name}: direct system Vulkan dependency bypasses the selected driver")
            raw_calls = {symbol for symbol in results[name]["strong_imports"] if re.match(r"vk[A-Z]", symbol)}
            require(not raw_calls,
                    f"{name}: unwrapped Vulkan imports bypass the selected driver: {', '.join(sorted(raw_calls))}")
        require("libadrenotools.so" not in results["libdriveclub_vulkan.so"]["needed"],
                "Vulkan selector must load AdrenoTools only after an explicit custom-driver choice")
        for name in ("libdriveclub_android.so", "libdriveclub_game.so"):
            require("libdriveclub_vulkan.so" in results[name]["needed"],
                    f"{name}: missing shared selected-driver dependency")
        commands_file = Path(__file__).resolve().parents[1] / "cmake/VulkanCommands.cmake"
        commands = set(re.findall(r"^\s+(vk[A-Za-z0-9_]+)\s*$", commands_file.read_text(), re.MULTILINE))
        require(commands, "Vulkan direct-call manifest is empty")
        wrappers = {"__wrap_" + command for command in commands}
        missing_wrappers = wrappers - results["libdriveclub_vulkan.so"]["exports"]
        require(not missing_wrappers,
                "Selected driver library is missing Vulkan forwarders: " + ", ".join(sorted(missing_wrappers)))
        for name, result in results.items():
            absent = set(result["needed"]) - set(present) - ANDROID_SYSTEM_LIBRARIES
            require(not absent, f"{name}: unsatisfied DT_NEEDED closure: {', '.join(sorted(absent))}")
            # ABI bridge imports must be satisfied by the library actually in this APK.
            cpu_imports = {symbol for symbol in result["strong_imports"] if symbol.startswith("guest_cpu_")}
            require(cpu_imports <= results["libbbcpu.so"]["exports"],
                    f"{name}: CPU bridge imports are absent from packaged libbbcpu.so")
            driver_imports = {symbol for symbol in result["strong_imports"]
                              if symbol.startswith(("driveclub_vk_", "__wrap_vk"))}
            require(driver_imports <= results["libdriveclub_vulkan.so"]["exports"],
                    f"{name}: selected-driver imports are absent from packaged libdriveclub_vulkan.so")
        require("libbbcpu.so" in results["libdriveclub_game.so"]["needed"],
                "Game JNI library is not linked to the packaged FEX backend")
        require("libaaudio.so" in results["libdriveclub_game.so"]["needed"],
                "Game JNI library is not linked to the Android AAudio backend")
        turnip = audit_turnip(archive)
        report = []
        for result in results.values():
            report.append({key: value for key, value in result.items() if key not in ("exports", "strong_imports")})
    return {"apk": str(apk.resolve()), "libraries": report, "bundled_driver": turnip,
            "selected_driver_forwarders": len(wrappers),
            "runtime_page_size_requirement": 4096,
            "validation": "ELF/link/package audit only; gameplay and Vulkan execution require device tests"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--json", action="store_true", help="Print the audit details as JSON")
    args = parser.parse_args()
    try:
        result = audit(args.apk)
    except (AuditError, OSError, ValueError, struct.error, zipfile.BadZipFile) as error:
        print(f"APK audit failed: {error}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for library in result["libraries"]:
            print(f"{library['name']}: AArch64/Bionic API {library['android_api']}, "
                  f"ELF alignment {library['load_alignment']}; DT_NEEDED {', '.join(library['needed'])}")
        driver = result["bundled_driver"]
        print(f"Bundled driver: {driver['name']}; ZIP SHA-256 {driver['zip_sha256']}; HAL {driver['hal_entry']}.")
        print(f"Selected Vulkan loader: {result['selected_driver_forwarders']} direct-call forwarders; no direct system loader dependency.")
        print("APK audit passed: ARM64 runtime, CPU/JNI exports, selected-driver closure and exact bundled Turnip.")
        print("ELF supports 16 KiB alignment; this renderer still requires a 4 KiB host page size.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
