#!/usr/bin/env python3
"""Exercise import identity/binding rules with synthetic ELF data, no game assets."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("import_audit", ROOT / "tools/audit-imports.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)
PROVIDER_SPEC = importlib.util.spec_from_file_location("provider_cases", ROOT / "tools/generate-provider-cases.py")
provider_cases = importlib.util.module_from_spec(PROVIDER_SPEC)
PROVIDER_SPEC.loader.exec_module(provider_cases)
NID = "0123456789A"


def elf(path, library="libExample", module="libExample", *, export=False,
        data=False, weak=False, scoped=True, version=1):
    strings = bytearray(b"\0")
    offsets = []
    for text in (library, module, NID + "#A#A" if scoped else "optional_hook"):
        offsets.append(len(strings))
        strings += text.encode() + b"\0"
    blob = bytearray(strings)
    blob += b"\0" * (-len(blob) % 8)
    symbols_at = len(blob)
    blob += bytes(24)
    blob += struct.pack("<IBBHQQ", offsets[2], ((2 if weak else 1) << 4) | (1 if data else 2),
                        0, 1 if export else 0, 0x100 if export else 0, 8 if data else 1)
    rela_at = len(blob)
    if not export:
        blob += struct.pack("<QQq", 0x180, (1 << 32) | (6 if data else 7), 0)
    tags = [(0x61000035, 0), (0x61000037, len(strings)),
            (0x61000039, symbols_at), (0x6100003f, 48),
            (0x61000029, rela_at), (0x6100002d, 0 if export else 24),
            (0x6100002f, rela_at), (0x61000031, 0),
            (0x61000013, (version << 32) | offsets[0]),
            (0x6100000d, (version << 32) | offsets[1]), (0, 0)]
    dynamic = b"".join(struct.pack("<QQ", *entry) for entry in tags)
    result = bytearray(0x400 + len(blob))
    result[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", result, 16, 3, 62, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    headers = [(1, 7, 0, 0, 0, len(result), len(result), 0x4000),
               (2, 4, 0x200, 0x200, 0, len(dynamic), len(dynamic), 8),
               (0x61000000, 4, 0x400, 0x400, 0, len(blob), len(blob), 8)]
    for i, header in enumerate(headers):
        struct.pack_into("<IIQQQQQQ", result, 64 + i * 56, *header)
    result[0x200:0x200 + len(dynamic)] = dynamic
    result[0x400:] = blob
    path.write_bytes(result)
    return path


class ImportAuditTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def test_exact_native_binding_and_data_kind(self):
        main = elf(self.root / "eboot.bin", data=True)
        native = elf(self.root / "native.prx", export=True, data=True)
        manifest = audit.collect_manifest([main, native], {NID: "example_data"})
        item, = manifest["imports"]
        self.assertEqual(item["runtime_name"], NID + "#A#A")
        self.assertEqual(item["kind"], "data")
        self.assertEqual(item["native_provider"]["module"], "native.prx")
        self.assertEqual(item["references"][0]["relocations"], ["0x180"])

    def test_different_library_version_cannot_satisfy_import(self):
        main = elf(self.root / "eboot.bin", version=1)
        native = elf(self.root / "native.prx", export=True, version=2)
        manifest = audit.collect_manifest([main, native], {})
        self.assertIsNone(manifest["imports"][0]["native_provider"])

    def test_libc_internal_unique_compatibility_binding(self):
        main = elf(self.root / "eboot.bin", "libSceLibcInternal", "libSceLibcInternal")
        native = elf(self.root / "libc.prx", "libc", "libc", export=True)
        manifest = audit.collect_manifest([main, native], {})
        self.assertEqual(manifest["imports"][0]["native_provider"]["module"], "libc.prx")

    def test_kind_mismatch_and_ambiguous_export_fail(self):
        main = elf(self.root / "eboot.bin", data=True)
        native = elf(self.root / "native.prx", export=True)
        with self.assertRaisesRegex(audit.AuditError, "kind mismatch"):
            audit.collect_manifest([main, native], {})
        main = elf(main)
        other = elf(self.root / "other.prx", export=True)
        with self.assertRaisesRegex(audit.AuditError, "ambiguous native export"):
            audit.collect_manifest([main, native, other], {})

    def test_unscoped_weak_symbol_is_zero_not_required_host_import(self):
        main = elf(self.root / "eboot.bin", weak=True, scoped=False)
        manifest = audit.collect_manifest([main], {})
        self.assertEqual(manifest["imports"], [])
        self.assertEqual(manifest["weak_unscoped_zero"][0]["name"], "optional_hook")
        main = elf(main, weak=False, scoped=False)
        with self.assertRaisesRegex(audit.AuditError, "required import without identity"):
            audit.collect_manifest([main], {})

    def test_truncated_and_oversize_elf_are_rejected(self):
        path = elf(self.root / "eboot.bin")
        full = path.read_bytes()
        path.write_bytes(full[:-1])
        with self.assertRaises(audit.AuditError):
            audit.parse_module(path)
        modified = bytearray(full)
        struct.pack_into("<Q", modified, 64 + 32, 1 << 60)
        path.write_bytes(modified)
        with self.assertRaisesRegex(audit.AuditError, "size limit"):
            audit.parse_module(path)

    def test_report_distinguishes_kind_and_compact_metadata_round_trip(self):
        main = elf(self.root / "eboot.bin")
        manifest = audit.collect_manifest([main], {NID: "example"})
        report = audit.audit(manifest, {NID: [{"kind": "data"}]})
        self.assertEqual(report["summary"]["required_missing"], 1)
        report = audit.audit(manifest, {NID: [{"kind": "function"}]})
        self.assertEqual(report["summary"]["required_missing"], 0)
        compact = audit.metadata_only(manifest)
        target = self.root / "manifest.json"
        audit.write_manifest(compact, target)
        self.assertEqual(json.loads(target.read_text()), compact)
        self.assertNotIn("relocations", compact["imports"][0]["references"][0])
        self.assertEqual(compact["imports"][0]["references"][0]["relocation_count"], 1)
        header = self.root / "cases.h"
        audit.write_header(compact, header)
        self.assertIn('"0123456789A#A#A", "example", 0, 0, 0', header.read_text())

    def test_comments_preserve_url_and_nid_literals(self):
        text = '"https://example.invalid"; /* removed\ncomment */ "nid#A#A"; // tail\n'
        cleaned = audit.uncomment(text)
        self.assertIn('"https://example.invalid"', cleaned)
        self.assertIn('"nid#A#A"', cleaned)
        self.assertNotIn("removed", cleaned)
        self.assertEqual(cleaned.count("\n"), text.count("\n"))

    def test_committed_provider_cases_match_port_sources_without_game_metadata(self):
        header = self.root / "cases.h"
        with mock.patch.object(provider_cases.audit, "parse_module", side_effect=AssertionError("module read")), \
             mock.patch.object(provider_cases.audit, "collect_manifest", side_effect=AssertionError("manifest collection")), \
             mock.patch.object(provider_cases.audit, "audit", side_effect=AssertionError("game import audit")):
            count = provider_cases.generate(ROOT, header)
        self.assertGreater(count, 0)
        self.assertEqual(header.read_bytes(), (ROOT / "tests/fixtures/runtime_provider_cases.h").read_bytes())

    def test_provider_cases_require_actual_declarations_and_preserve_kind(self):
        rows = provider_cases.cases({NID: [dict(function="example", kind="function")]*2,
                                     "ABCDEFGHIJK": [],
                                     "LMNOPQRSTUV": [dict(function="example_data", kind="data")]},
                                    {NID: "example_public_api", "ABCDEFGHIJK": "unimplemented_api"})
        self.assertEqual(rows, [dict(name=NID, symbol="example_public_api", is_data=0),
                                dict(name="LMNOPQRSTUV", symbol="example_data", is_data=1)])
        with self.assertRaisesRegex(provider_cases.audit.AuditError, "invalid source provider NID"):
            provider_cases.cases({"not-a-valid-nid": [dict(function="example", kind="function")]}, {})

    def test_thread_resolver_accepts_bare_and_scoped_nids(self):
        providers = audit.source_providers(ROOT, {})
        # pthread_detach is a bare NID in the real table, whereas the kernel
        # spelling is scoped. Both are explicit providers, not unresolved APIs.
        self.assertTrue(providers["+U1R4WtXvoc"])
        self.assertTrue(providers["4qGrR6eoP9Y"])


if __name__ == "__main__":
    unittest.main()
