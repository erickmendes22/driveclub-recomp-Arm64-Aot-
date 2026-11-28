# Runtime provider tests and private import diagnostics

The public resolver tests describe the port's implementation. A separate local
tool can inspect a user's decrypted PS4 modules. The two workflows have different
inputs: no game inventory, module hashes, module identities, references or binary
offsets are inputs to the public fixture generator or CI.

## Public provider fixture and CI

After changing runtime bindings or a registered GPU/network provider, regenerate
the source-only fixture and run the tests:

```sh
python3 tools/generate-provider-cases.py \
  --output tests/fixtures/runtime_provider_cases.h
python3 tests/test_import_audit.py
```

`generate-provider-cases.py` accepts only a source root and an output path. It
reads the port's `RuntimeExport`, direct NID bindings, registered GPU/network
`LIB_FUNCTION`/`LIB_OBJ` declarations and the existing runtime name table. It
includes only identities with an actual provider declaration; an entry in the
name table alone does not count. It never opens game modules or a private
manifest. Its generated public header contains:

```c
typedef struct {
    const char *name, *symbol;
    int is_data;
} RuntimeProviderCase;
```

`name` is a canonical NID from the implementation. `symbol` is its public API or
provider function name, and `is_data` distinguishes data from function bindings.
The array is `runtime_provider_cases`; `RUNTIME_PROVIDER_CASE_COUNT` gives its
length. Duplicate declarations of the same NID and kind produce one case.

`tests/test_import_audit.py` checks the committed header against current port
sources and exercises import parsing with small synthetic ELF files constructed
inside temporary directories. It requires no game files or private metadata.
This catches a stale fixture when another provider, including capture services,
is added. Keep the generated header in the same change as its source bindings.

`tests/test_android_imports.cpp` loads the real `libdriveclub_game.so` from the
APK with its actual Bionic dependency closure. It registers the providers without
starting Vulkan or network services, resolves every source-declared case, and
fails if one is absent. It also checks that registration is idempotent, starts no
threads, and does not make an unknown NID resolve to a fabricated provider.

The source scanner uses the Android C runtime and an explicit list of GPU/network
registry sources called by the registration code. A new, unregistered GPU source
file does not count. Keep that list synchronized when adding a registry.

These checks establish source/linked registration consistency. They do not
establish coverage of any game's imports, correct behavior of every provider,
callback ABI correctness or rendered gameplay. Contract tests and the FEX ABI
tests cover separate behavior. The loader's preflight checks the actual locally
prepared image and reports imports still absent from both HLE and native modules.

## Optional private local module audit

`audit-imports.py` reads relocated imports and their complete library/module
identities from original decrypted PS4 SELF/ELF files. It reproduces the native
binding rules in `launcher/prepare.cpp`; it does not prepare, modify or execute
a game image.

**Module-derived output is local analysis only.** Keep reports, manifests and
optional case headers under the ignored `out/private-import-audit` directory or
outside the checkout. Do not add them to the repository, releases or CI
artifacts. They are not prerequisites for any public build or test.

Provide exactly one intended version of `eboot.bin` and each module. Duplicate
filenames are rejected so a base/update collision cannot silently change the
inventory. For example, with privately extracted inputs:

```sh
python3 tools/audit-imports.py \
  --modules /path/to/private/modules \
  --write-manifest out/private-import-audit/manifest.json --metadata-only \
  --output out/private-import-audit/report.json --list-missing

# Recheck the private inventory after changing providers, without rereading ELF:
python3 tools/audit-imports.py \
  --manifest out/private-import-audit/manifest.json \
  --output out/private-import-audit/report.json --list-missing
```

The private manifest records identities, hashes, native binding information and
reference counts. Without `--metadata-only`, it also records relocation offsets,
PLT addresses and candidate direct call sites. Those byte-pattern candidates do
not prove that a call is reachable from startup. `--header` can emit an additional
private diagnostic header; keep it in the same private output directory and do
not substitute it for the public source-only provider fixture.

The local audit distinguishes a host source declaration, a native module binding
and an import for which neither was found. A native binding requires the complete
library/module identity and version; the one compatibility rule is the preparer's
unique `libSceLibcInternal` to `libc` fallback. Data/function kind mismatches and
ambiguous native exports fail the inventory. Additional modules need a new local
inventory or the loader's actual-image preflight.

`--require-complete` returns status 1 when any referenced strong import has no
provider. Ordinary diagnostics return 0; malformed input or missing source files
return 2. A strong ELF import does not by itself mean an offline gameplay path
calls that function. Keep known unavailable features explicit, and do not hide
an absence by registering a generic successful stub.
