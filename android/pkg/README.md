# Local PKG installer

This library uses the pinned GoW3 Bionic Orbis PKG/PFS parsers, built as a Rust
static library and called through `pkg_bridge.cpp`. The Android app never
executes an extracted helper binary. See `vendor/SOURCE.md` for the source pin
and licenses; registry dependencies are pinned by `Cargo.lock`.

`PkgInstaller.inspect(Context, Uri)` validates the read-only SAF file descriptor,
package header, title `CUSA00093`, PARAM.SFO, supported patch flags, RSA key
envelopes, encrypted outer PFS, compressed PFSC container, inner PFS tree and
output paths. `install` rechecks the selection fingerprint, extracts into a
unique empty private staging directory, then atomically renames that generation.
The caller publishes the resulting root only after this succeeds. Updates are
separate overlays and do not duplicate an existing base `game000.dat` or touch
saves. Progress callbacks run on the installation worker and return false to
cancel. Errors/cancellation discard only the new staging content.

The source descriptor must provide a regular seekable, mappable local file.
Input mapping is read-only and demand-paged; the original PKG is not copied to
app storage. The selection fingerprint covers metadata, size and modification
time; it is not a full-package content hash or Sony signature verification.

Supported packages are complete FPKGs whose keys and PFS layout are supported
by the vendored parser. Retail-specific keys, delta patches requiring another
version's blocks, split/multiple-image packages and 64-bit PFS inode layouts are
rejected. Bounds limit a package to 512 GiB, extracted content to 256 GiB,
metadata to 64 MiB and the directory tree to 64 levels. Unsupported inputs
produce an error before publishing any replacement installation.

Build and test:

```sh
bash android/tools/build-pkg.sh --test-only
bash android/tools/build-pkg.sh --fixtures
ANDROID_NDK_HOME=/path/to/ndk bash android/tools/build-pkg.sh --abi arm64-v8a
ANDROID_NDK_HOME=/path/to/ndk bash android/tools/build-pkg.sh --abi x86_64
```

The helper pins Rust 1.85.0. It uses `out/pkg-tests` for host outputs and
`out/android-pkg/<rust-target>/release/libdriveclub_pkg_core.a` for Android.
The two small fixtures in `out/pkg-fixtures` contain only synthetic metadata and
text bytes, wrapped in actual RSA/AES/PFS/PFSC formats. They are intended only
for Android test assets. The sparse >4 GiB fixture is generated inside the host
test temporary directory and is never added to an APK.

Host tests cover encrypted/compressed extraction, a separate update overlay,
preservation of an existing destination/save, wrong-title and unsupported-key
rejection, delta/truncated packages, unsafe paths, allocation bounds,
cancellation, corrupt file data, a changed inspection fingerprint and a PFS
offset above 4 GiB. Passing these tests does not establish that a real Driveclub
PKG has been installed or that its game code boots on Android.
