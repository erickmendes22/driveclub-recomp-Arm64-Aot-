# Building

The game runtime and the launcher are built on Windows with the MSYS2 CLANG64 toolchain. Everything else
the code needs is in this repository (`gpu/third_party/`, `third_party/`).

## Toolchain

Install MSYS2 and, from the CLANG64 shell, the build packages once:

    pacman -S --needed mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,vulkan-headers,vulkan-loader,\
    vulkan-memory-allocator,fmt,boost,robin-map,xxhash,sdl3,zydis,ffmpeg,protobuf,nlohmann-json,\
    openssl,zlib,miniupnpc}

## Game runtime

    bash build.sh

This produces `out/DriveClubGame.exe` without diagnostic capture/profiling code.
For a separate diagnostic executable, use `BB_DIAGNOSTICS=ON bash build.sh`; see
[Runtime diagnostics builds](docs/runtime-diagnostics.md).

## Release

    bash packaging/package.sh 1.0

This builds the runtime and the launcher, copies the runtime libraries next to them and writes
`release/driveclub-recomp-1.0-win64.zip`. The zip needs nothing installed on the player's machine.

## CI

The Linux workflow needs no MSYS2 on the developer's machine and can be started with
**Run workflow** (`workflow_dispatch`).

- `.github/workflows/appimage.yml` (`ubuntu-24.04`) builds the native Linux runtime with
  `build-linux.sh` (which compiles `src/runtime*.c`) and runs `tools/font_cjk_fallback_test.c`.
  This is the fast compile/regression gate for the HLE sources.

`DriveClubGame.exe` is a WIN32-only CMake target that links `onecore`, `bcrypt` and `ws2_32`,
so a native Linux runner cannot produce it; build it locally with `bash build.sh` as described
in [Game runtime](#game-runtime).

### CJK font fallback

`src/runtime_font_fallback.inc` resolves a glyph missing from the game's primary font against a
shared CJK face. The face is loaded once, on the first missing glyph, from the first of:

1. `BB_FONT_CJK_FALLBACK` — an absolute host path or a `/app0/...` guest path; `0` or `off`
   disables the fallback.
2. `/app0/newui/fonts/tradchinese-dfheistdw7-gb.ttf` and the other CJK faces in `newui/fonts`,
   resolved through the game's path translator (so a `mods/` copy is picked up).
3. `mods/newui/fonts/...` or `newui/fonts/...` relative to the working directory, for when the
   guest-side miss cache has already marked `/app0/newui/fonts` as absent.

Run the regression test from the repository root (no game files needed):

    cc -O2 -pthread -I. -Isrc tools/font_cjk_fallback_test.c -lm -o out/font_cjk_fallback_test
    out/font_cjk_fallback_test
    BB_TEST_CJK_FONT=/path/to/a-cjk.ttf out/font_cjk_fallback_test

## Linux (x86_64)

The Linux support comes from the upstream ARM Linux branch
([seregasmith/ARM_linux_driveclub-recomp](https://github.com/seregasmith/ARM_linux_driveclub-recomp),
branch `arm_linux`), which runs the game natively on ARM64 through FEXCore. On x86_64 the game's
code runs on the host CPU: `src/guest_cpu.h` resolves to plain calls in `src/runtime_cpu.c`, and
no FEX library is built. CMake builds `libbbgpu.so`; the loader/HLE runtime is compiled from
`src/` (`src/compat_win.c` is Windows-only). The Win32/WPF launcher is not part of the Linux
build; `driveclub-prepare` (from `tools/prepare_cli.cpp`, using the portable
`launcher/prepare.cpp`) prepares the game dumps, and the AppImage's `AppRun` starts the runtime
with the prepared files.

### Dependencies

Ubuntu 24.04 or newer:

    sudo apt install build-essential clang cmake ninja-build pkg-config curl ca-certificates \
      libvulkan-dev libfmt-dev libboost-dev libxxhash-dev libzydis-dev \
      libprotobuf-dev protobuf-compiler nlohmann-json3-dev \
      libssl-dev zlib1g-dev libpng-dev libminiupnpc-dev \
      glslang-tools libx11-dev libxext-dev libxrandr-dev libxi-dev \
      libxcursor-dev libxfixes-dev libxinerama-dev libxss-dev libxtst-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev \
      libxkbcommon-dev libudev-dev libdrm-dev \
      nasm squashfs-tools

Ubuntu 24.04 has no `libsdl3-dev` or `libtsl-robin-map-dev`, and its Vulkan headers are older
than the video core (`VK_KHR_maintenance8`, `VK_EXT_robustness2`). The AppImage dependency
script builds SDL3 (with ALSA/PulseAudio/PipeWire) and a minimal FFmpeg, and installs the
Vulkan headers, robin-map and the Vulkan Memory Allocator header under `out/deps`:

    bash packaging/appimage/fetch-deps.sh

On a distribution that packages SDL3, FFmpeg, the newer Vulkan headers, robin-map and the
Vulkan Memory Allocator header, that step can be skipped; `build-linux.sh` and CMake pick up
whatever `pkg-config` and the CMake package registry find.

### Runtime

    bash build-linux.sh

This produces `out/driveclub-probe`, `out/gpu/libbbgpu.so`, `out/driveclub-prepare` and
`out/driveclub-launcher`. The launcher is the native Linux equivalent of the Windows launcher
(SDL3 + the ImGui already in `gpu/third_party`): it picks the game folders, edits the recomp
settings and patches, prepares the game and starts it. For the AppImage it is the default
entry point; run it directly with `out/driveclub-launcher`, or prepare the dumps headlessly
and start the runtime from a directory that holds `trophies.tsv` (the runtime reads it from
the working directory):

    mkdir -p data
    cp packaging/release/trophies.tsv .
    out/driveclub-prepare --update /path/to/update --base /path/to/base \
      --work data --patches patches/DriveClub.xml
    out/driveclub-probe data/prep/boot-linked.bin --content-profile data/prep/content.bin \
      --patches data/prep/patches.bin --app0 /path/to/update --app0-base /path/to/base \
      --user data/user --mods data/mods --stub-unresolved --timeout 0

The launcher writes the runtime's `BB_*` environment variables (and `camera.ini`,
`picture.ini`, `driveclub.ini`, `patches_enabled.txt`) from its settings and saves them in
`DRIVECLUB_DATA_DIR` (the AppImage uses `data/` beside itself when that folder is writable,
otherwise `~/.local/share/driveclub-recomp`); they are the same options documented in the
README.
`BB_RESTART_COMMAND` lets the in-game settings restart the runtime (the AppImage sets it to
the AppImage itself with `--play`).

### AppImage

    bash packaging/appimage/build-appimage.sh 0.4.0

This builds the runtime and the native Linux launcher (unless `SKIP_BUILD=1`), stages an
AppDir and writes `release/driveclub-recomp-0.4.0-x86_64.AppImage` and its `.sha256`. Running
the AppImage without arguments opens the launcher; `--play` skips it. The launcher keeps its
data in `data/` beside the AppImage when that folder is writable, like the Windows build, and
otherwise in `~/.local/share/driveclub-recomp`. DATA FOLDER > Change... selects another
folder (moving or copying the existing data); the choice is saved in `data-path` beside the
AppImage (or in the XDG config directory for a read-only install). `DRIVECLUB_DATA_DIR`
overrides it; `DRIVECLUB_PORTABLE=1` or a `portable` marker selects `data/` when no folder has
been chosen. It needs
`squashfs-tools` and network access once for the AppImage type-2 runtime (kept in
`out/appimage/`). Useful variables:
`APPIMAGE_COMP` (default `zstd`), `APPIMAGE_ZSTD_LEVEL` (15), `APPIMAGE_BLOCK` (128K) and
`APPIMAGE_RUNTIME` for an already downloaded runtime.

The package bundles SDL3, the minimal FFmpeg, fmt, protobuf, zydis, libpng, the launcher's
ImGui code, its bundled interface font and the C++ runtime. glibc, the Vulkan
loader/ICD/driver, the X11 stack (through XWayland on Wayland desktops), the font stack and the
audio backends stay the host's (bundling them breaks the driver and the desktop). It is built on
Ubuntu 24.04, so it needs glibc 2.39 or newer on the player's machine.

`.github/workflows/appimage.yml` builds the AppImage on `ubuntu-24.04` for tag pushes
(`v*`), manual runs and pull requests that touch the pipeline, extracts the built AppImage and
smoke-tests it (`--version`, `--selftest`, `--help`), failing on unresolved shared libraries,
and uploads it as an artifact.

## Finding a stall

These switches show where the frame time goes. They are off unless set, and cost nothing when off. Set them
in the environment of `DriveClub.exe --play`; with `BB_BENCHMARK` (see the README) the race is the same every
time.

- `BB_FRAME_STATS=1`: every 5 seconds the log gets the flip rate and frame-time spread, where the GPU command
  thread waited, the guest reads of GPU-written data (offloaded, early copies made and used), the faults
  emulated and declined, and the label waits.
- `BB_FRAME_LOG=<file>`: one line per flip, the wall-clock time and the interval since the previous flip (ms).
  For the 1% and 0.1% lows and the spikes.
- `BB_FRAME_TRACE=<file>`: one line per flip with where that frame's time went. The columns are named in the
  first line: the PrepareFrame and frames-ahead waits, GPU thread idle time, the submission lock, read faults
  (count, total, the worst thread), write faults, the present, shader compiles, texture evictions, uploads and
  draws.
- `BB_READBACK_LOG=<file>`: one line per guest read that downloads GPU-written data: the faulting instruction
  and address, how long the read waited for the GPU thread, for the submission that wrote the data and for
  the copy, and the shader or copy that wrote it.

A frame that misses its slot shows in the frame log as a long interval. Its line in the frame trace says whether
the time went to read faults (the readback log then names the instruction and the writer), shader compiles,
evictions or the GPU.

## Seamless cubemap filtering

Native Vulkan cube-array sampling is enabled automatically for eligible read-only,
floating-point cubemap resources. This corrects reflection seams caused by filtering each
cube face independently as a 2D-array layer. No environment variable or launch option is
required.

The initial implementation handles normalized explicit-LOD samples (including level 0).
Storage, depth, integer, offset, gather, derivative and mixed/raw uses retain the array path,
as do samplers that disable cube wrapping. The GPU must support `imageCubeArray`; eligible
views must contain complete aligned cube faces. The shader/pipeline cache versions invalidate
older translated shaders and resource layouts, including the earlier opt-in variants.

### Regression checks (MSYS2 CLANG64, from the repository root)

Build the runtime normally with `bash build.sh`, then run:

```sh
clang++ -O2 -std=c++20 tools/native_cube_coords_test.cpp -o out/native_cube_coords_test.exe
out/native_cube_coords_test.exe
clang++ -O2 -std=c++20 -Igpu/third_party/sirit/include \
  -Igpu/third_party/sirit/externals/SPIRV-Headers/include \
  tools/native_cube_shader_test.cpp out/gpu/third_party/sirit/src/libsirit.a \
  -lfmt -o out/native_cube_shader_test.exe
out/native_cube_shader_test.exe out/native_cube_test.spv
spirv-val --target-env vulkan1.1 out/native_cube_test.spv
```

The coordinate test evaluates the production emitter helper against independent cube-face
bases at centers, edges, corners and multiple array indices (2,430 cases). The shader test
uses the same helper in a compute shader with both 2D-array and cube-array sampling, then
validates the generated SPIR-V. These checks require no game files or captured resources.

## Faster release build (profile-guided)

The player release is built with link-time optimization and a recorded profile (`pgo/default.profdata`): `packaging/package.sh` does this by itself when the profile is there. To build it by hand: `BB_PGO=use BB_LTO=ON bash build.sh`. The default `bash build.sh` stays the quick build.

To record a new profile after the code has changed a lot: build with `BB_PGO=generate BB_LTO=ON`, run the game's benchmark and a race or two with your usual settings (the profile is written every 30 seconds into `pgo/`), then merge the `.profraw` files with `llvm-profdata merge -output=pgo/default.profdata pgo/*.profraw`.

## Android ARM64 (Bionic and FEX)

The Android 0.6 target integrates the local preparer, Driveclub loader/HLE, FEXCore,
the existing shadPS4-based renderer, local FPKG installation, a bundled Turnip
selector and analogue touch controls. It targets `arm64-v8a`, Android 11/API 30
or later, using Bionic throughout. The 0.5 APK audit, seven host PKG tests,
six Android integration tests and five native ARM64/Bionic FEX checks passed.
Building the APK or passing synthetic tests does not
establish that the original game reaches its menu or gameplay; that requires an
Android device run with the user's game files and its startup log.

The supplied 0.5 device log confirms local preparation of update 1.28, loading
Turnip Gen8 V39-patched on Adreno 840, creation of the native Vulkan swapchain,
initialization of all seven boot modules and entry into the original game code.
It then records failed mappings for FlexHeap and MagicMemory, followed by a native
fault when the game calls `memmove` into the unmapped FlexHeap. That run does not
establish menu or gameplay rendering.

### Build

Use a Linux x86_64 build host with JDK 17 or 21, Android SDK 35, Build Tools 35.0.0,
NDK **29.0.14206865**, and CMake **3.31.6**. The scripts also need Git, a host C/C++
compiler, Ninja, pkg-config, Python 3, curl, tar, make, Perl, unzip and Rustup. The
PKG helper installs and explicitly selects **Rust 1.85.0** with the locked Cargo
dependencies, independently of the host's default Rust version. Install the
Android packages through Android Studio or `sdkmanager`:

    sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;29.0.14206865" "cmake;3.31.6"

From the repository root:

    export ANDROID_HOME=/path/to/android-sdk
    export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/29.0.14206865"
    export PATH="$ANDROID_HOME/cmake/3.31.6/bin:$PATH"
    export CMAKE_BUILD_PARALLEL_LEVEL=3
    android/tools/build-fex.sh
    android/tools/fetch-gpu-deps.sh
    bash android/tools/build-pkg.sh --abi arm64-v8a
    python3 android/tools/fetch-turnip.py
    (cd android && ./gradlew assembleDebug)
    python3 android/tools/verify-apk.py android/build/outputs/apk/debug/driveclub-android-debug.apk

`build-fex.sh` builds `out/android-fex/build/libbbcpu.so`. Despite its name,
`fetch-gpu-deps.sh` also cross-compiles the renderer, media and network dependencies
under `out/android-gpu/`; it keeps host shader/protobuf tools separate from Android
libraries. `build-pkg.sh` creates
`out/android-pkg/aarch64-linux-android/release/libdriveclub_pkg_core.a`; CMake links
it into `libdriveclub_pkg.so` through JNI. No extractor executable is launched on
Android. `fetch-turnip.py` verifies the exact upstream ZIP and stages it under
`out/android-driver/assets/`; an offline copy can be supplied with
`--archive /path/to/Turnip_Gen8_V39_Patched.zip` and undergoes the same checks.
Gradle compiles and packages the launcher, runtime, FEX, installer and driver
loader. No game files are needed to build the APK. Install it with
`cd android && ./gradlew installDebug` or `adb install -r` using the APK path above.

The **Android Bionic FEX runtime** GitHub Actions workflow
(`.github/workflows/android-bootstrap.yml`) can also be started with **Run workflow**.
It builds and audits the APK and archives it as
`driveclub-android-bionic-fex-arm64-debug`.

### Signing APKs for updates

GitHub Actions debug builds use a temporary runner's signing key. Distributed
builds from 0.5 onward are re-signed with one retained private key so later APK
updates can preserve the app's installed packages, saves and settings. Keep the
private `Driveclub-Android-private-signing.zip` backup outside the repository and
reuse that key; never publish its keystore or password in source or CI artifacts.
Its public certificate SHA-256 is
`46068ca7ce4ba21906c06b1b65cbf28ca0dcbbfd16406e67a83c66ecc943eb5b`.

With the backup extracted in a private directory, use the SDK signing tool:

    "$ANDROID_HOME/build-tools/35.0.0/apksigner" sign \
      --ks /private/driveclub-android-release.p12 \
      --ks-key-alias driveclub-android \
      --ks-pass file:/private/store-password.txt \
      --v4-signing-enabled false \
      --out /output/Driveclub-Android-signed.apk /input/driveclub-android-debug.apk
    "$ANDROID_HOME/build-tools/35.0.0/apksigner" verify --verbose --print-certs \
      /output/Driveclub-Android-signed.apk
    python3 android/tools/verify-apk.py /output/Driveclub-Android-signed.apk

The previously distributed 0.4 APK used a different, unavailable debug key. It
cannot be upgraded in place with this key: replacing that version requires a
one-time uninstall, which removes its private app data. Export any needed logs
first. Game folders selected outside the app's private storage are not removed
by that uninstall. Subsequent distributed APKs must retain the new signing key.

### Local preparation and runtime

In **Biblioteca**, select **Pasta do jogo** containing `game000.dat`. A folder
already merged with update 1.28 needs no second selection: the effective update
source is the game folder itself. **Atualização separada (opcional)** supplies
an update-first overlay only when the update lives elsewhere. Folder sources and
locally installed PKGs share the same read-only runtime file API.

The app retains read-only Storage Access Framework (SAF) grants and runs the
shared `launcher/prepare.cpp` preparer on the device. The executable and modules
are staged in a private generation; boot, content and patch files are created
there. No prepared boot image is downloaded or distributed with the APK.
`game000.dat` stays in its selected folder: preparation uses an open descriptor,
and runtime reads/seeks use native descriptors opened through SAF. The provider
must support seekable files; use a local device folder rather than a provider
that exposes only streams.

The last complete generation remains available if preparation fails. Saves and user
data live in the separate private `driveclub/user` directory, outside `local-*`
generations, so replacing a prepared generation does not remove saves.

### Local PKG installation and recovery

Use **Instalar / atualizar por PKG** to inspect a local package and confirm its
title, version, type and required extraction space. The installer accepts
Driveclub **CUSA00093**; the launcher requires update **1.28** for this runtime.
A base PKG may be installed first, followed by its update. The input descriptor
is read directly, and the package is not copied into private storage before
extraction.

Extraction writes a new `.pkg-staging-*` directory under the app's private
`driveclub/installed` root. Cancellation and extraction errors remove only that
staging; the next installation also cleans staging left by a terminated process.
Completed extraction is renamed atomically to a `base-*` or `update-*` generation
before the library selection is published. An update remains a separate overlay
and does not copy the base `game000.dat`.

The previous selection and installed files remain available until the new local
preparation succeeds. **Restaurar seleção anterior** restores that selection if
preparation fails; after success, unused old package generations can be reclaimed.
Cleanup failures are logged and do not turn a published success into a failure.
Saves remain in `driveclub/user` throughout.

The backend parses real FPKG key envelopes, AES-encrypted PFS and PFSC/zlib data
using the vendored `orbis-pkg`/`orbis-pfs` libraries. It supports the public fake
package keysets; it does not supply console-specific retail keys. Unsupported
encryption, delta patches needing a base filesystem, multiple PFS images and
64-bit PFS inode layouts are rejected. Package paths and parser allocation sizes
are bounded, and special files/path traversal are rejected. PFS signature/hash
verification is not provided by this parser; a successful metadata inspection
is not a full-package integrity proof. Files and package offsets above 4 GiB
are supported within the integration's size limits.

### Graphics selection and controls

The ARM64 build defaults to **Turnip Gen8 V39-patched**. In **Sistema → Processador
gráfico → Escolher driver**, select the included Turnip or **Driver do sistema**.
The x86_64 launcher test build defaults to the system driver and cannot load the
ARM64 Turnip library. The app bundles the exact `Turnip_Gen8_V39_Patched.zip`
release asset as `assets/drivers/turnip-gen8-v39-patched/driver.zip`, preserving
the upstream ZIP and checking its SHA-256 before installation. It contains
`libvulkan_freedreno.so`; it is a Vulkan HAL loaded through the pinned AdrenoTools
integration, not a replacement system `libvulkan.so`.

The launcher probe and game renderer use the selected loader, including direct
Vulkan calls. A failed custom-driver load is reported and blocks that choice;
it never silently switches to the system driver. Choosing the system driver
explicitly remains available. Bundling Turnip does not establish compatibility
with a particular GPU or Android version; the exact asset's ABI note and release
metadata differ, as recorded in its `NOTICE.txt`.

On-screen racing controls provide analogue steering, separately adjustable
accelerator/brake pedals and simultaneous multitouch. The menu toggle exposes
the controller buttons and menu/navigation controls; switching layouts, losing
focus or cancelling a touch stream releases held inputs. Physical Android
gamepads also feed the pad backend. **Sistema → Controles do jogo → Testar
controle na tela** opens the same controls with live values without starting
the game.

`GameActivity` runs in the dedicated `:runtime` process. Its JNI library enters the
loader, FEX executes the original x86-64 code, and the renderer presents through
`ANativeWindow` and Vulkan. Audio uses AAudio. A native exit or crash is contained in that
process so the garage can be reopened. Android IME dialogs, steering-wheel support,
force feedback and controller vibration are not implemented in this backend.

In **Sistema → Diagnóstico**, **Ver logs** displays the history and **Exportar logs**
saves `driveclub-diagnostico.txt`. The export includes launcher history and the current
and previous native runtime logs. Reopen the app after a crash to export them; Android's
previous-exit reason is included when the OS supplies it.

### Device requirements and limits

Game execution currently requires **4 KiB host pages**. The runtime rejects other
page sizes before entering the loader because renderer memory tracking still uses
4 KiB pages. The native ELF libraries use 16 KiB load alignment, and the downstream
FEX guard-page patch uses the actual host page size; neither establishes renderer
support for a 16 KiB Android kernel.

The real renderer requires **Vulkan 1.3**, an Android presentation surface, and its
required device extensions: `VK_KHR_swapchain`, `VK_KHR_push_descriptor`,
`VK_EXT_vertex_attribute_divisor` and `VK_EXT_robustness2`. The latter must expose
`robustBufferAccess2`, `robustImageAccess2` and `nullDescriptor`. A successful launcher
Vulkan probe alone does not prove that those renderer requirements are satisfied.

### Verification

#### Android 0.6 memory regression

The failing device run mapped MainHeap at `0x2000000000` (530 MiB) and
VideoPrivate at `0x5000000000` (3674 MiB), but failed to map FlexHeap at
`0xf000000000` (338 MiB) and MagicMemory at `0xa000000000` (176 MiB). The
successful addresses are below 512 GiB and the failed ones are above it. This
is consistent with a 39-bit host address space; the old log discarded the
underlying `mmap` errno, so it does not independently measure the device's VA width.

Inspection of the supplied 1.28 executable confirms that the four boot mappings
pass guest flags `0`, and the game reloads the bases through the output pointers
before constructing its heaps. Those bases are hints, not fixed guest mappings.
The faulting return offset `0x7eb837` follows a `memmove` call whose destination
comes from the heap allocator. The correction therefore belongs in the runtime's
non-fixed mapping path; it does not require patching the game executable or
rewriting guest pointers.

Version 0.6 first tries the guest's non-fixed hint with `MAP_FIXED_NOREPLACE`.
If that address is unavailable, it searches from 384 GiB, skips both guest and
native host mappings, and confirms the chosen range with the same atomic
no-overwrite operation. This keeps the 64 GiB fixed PRT alias window available.
Only a successful mapping changes the guest's output pointer; fixed requests
retain their exact-address semantics. The direct and flexible budgets are
unchanged. Failure logs now retain the host errno, range, protection and backing
offset, and a successful change records both the hint and the returned address.

The regression tests reproduce the four full heap sizes with only the host
mapping failures above `2^39` simulated. Successful mappings and backing storage
remain real. The ARM64 test links the same Bionic runtime archive as the app and
uses FEX from the built APK: x86-64 code reloads the returned FlexHeap address,
calls Bionic `memmove` at `FlexHeap + 0x020000cc`, and verifies the same bytes
through the renderer's backing-memory API. It also checks disjoint heaps, the
unchanged 5056 MiB direct-memory budget, 2 MiB direct alignment, the fixed alias
at 64 GiB and release of its shared backing. No game executable or assets are
included in these tests. The simulated ceiling is a linker wrapper in the test
only, not an APK setting.

The previous 0.5 evidence below remains useful for features unchanged by the fix.

The 0.5 validation records are the
[ARM64 APK audit and native Bionic/FEX run](https://github.com/erickmendes22/driveclub-recomp-Arm64-Aot-/actions/runs/46082315169)
and the [Android 15 integration, controls and log-export job](https://github.com/erickmendes22/driveclub-recomp-Arm64-Aot-/actions/runs/46083788175/job/140788571573).
The delivered app was built at `bb85d0b4286207c3edc8efb4412dfaac1d75fadf`;
subsequent changes for the successful emulator run affect only the smoke script
and instrumentation diagnostics, not the app's code or assets. After private
signing, the APK was audited again and its non-signature payload matched the CI
APK. The delivered APK is 17,209,904 bytes, SHA-256
`9edb86a8540365b1dde24f308e7aa8c57fa08394774c1e4c10539efbd479ccf2`.

Run the host contract tests from the repository root without game files:

    mkdir -p out/android-tests
    c++ -std=c++20 tests/test_update_metadata.cpp launcher/prepare.cpp -o out/android-tests/test-metadata
    out/android-tests/test-metadata
    cc -std=c11 -D__ANDROID__ -DDRIVECLUB_ANDROID_RUNTIME=1 -I. -Isrc \
      tests/test_android_runtime_io.c src/runtime_file.c src/runtime_pad_android.c \
      -pthread -lm -o out/android-tests/test-android-io
    out/android-tests/test-android-io
    cc -std=c11 -O2 -ffunction-sections -fdata-sections -pthread \
      tests/test_runtime_memory.c -Wl,--gc-sections -o out/android-tests/test-runtime-memory
    out/android-tests/test-runtime-memory

These cover metadata, large-file descriptor I/O, directory/stat handling, read-only
content, save paths, pad input, and virtual-range naming/bounds/backing offsets.
`verify-apk.py` audits the actual packaged ARM64 ELF files, JNI/CPU exports, Android
dependency closure, selected-driver linkage, the exact bundled Turnip asset and
16 KiB load alignment; it also rejects packaged game data.

Run the real-format package tests and generate the Android test assets:

    bash android/tools/build-pkg.sh --test-only
    bash android/tools/build-pkg.sh --fixtures
    bash android/tools/test-vulkan-dispatch.sh

The seven Rust tests exercise encrypted/compressed extraction, update overlays,
wrong title/unsupported formats, parser bounds, cancellation/corruption rollback,
offsets beyond 4 GiB and changed-input rejection. They passed locally during 0.5
development. The generator writes three 384 KiB synthetic PKGs to
`out/pkg-fixtures`: base, update and an extraction-stage corrupt case. These
contain no game code and are included only in the instrumentation test APK.
The Vulkan dispatch test uses controlled host loaders to check selected-driver
routing, direct-call forwarding, switching/lifetime and blocked silent fallback;
it does not run the actual Turnip GPU driver.

For the Android 15 x86_64 emulator integration tests, build the matching installer
archive and test APK (the game runtime is intentionally ARM64-only):

    bash android/tools/build-pkg.sh --abi x86_64
    python3 android/tools/fetch-turnip.py
    (cd android && ./gradlew assembleDebug assembleDebugAndroidTest -PlauncherTestAbi=x86_64)
    (cd android && ./gradlew connectedDebugAndroidTest -PlauncherTestAbi=x86_64)

Run `--fixtures` first as above. The six instrumented tests comprise three real
touch-stream/control tests and three JNI package/source tests: simultaneous
steering and pedals, independent pointer release, focus/cancel/menu transitions,
base/update publication, cancellation/corruption recovery, saves preservation and
a unified folder with no separate update. All six passed on the Android 15
emulator during 0.5 validation; they do not execute the original game or validate
device rendering. The workflow
also runs the launcher/log-export smoke checks through `android/smoke-launcher.sh`.

Build the FEX ABI test against the backend extracted from the APK:

    android/tools/build-runtime-core.sh
    mkdir -p out/fex-test
    unzip -jo android/build/outputs/apk/debug/driveclub-android-debug.apk \
      'lib/arm64-v8a/libbbcpu.so' 'lib/arm64-v8a/libc++_shared.so' -d out/fex-test
    "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang++" \
      -std=c++20 tests/test_android_fex.cpp out/android-runtime/libdriveclub-runtime.a \
      -Wl,--gc-sections -Wl,--wrap=mmap -Wl,-z,max-page-size=16384 -Lout/fex-test -lbbcpu -o out/fex-test/test-fex

For a host-side run, obtain the pinned official Android API 30 ARM64 Bionic runtime
with `fetch-bionic-runtime.py`. Extraction needs `debugfs` from `e2fsprogs` and about
4 GB of temporary disk space. An x86_64 host also needs `qemu-aarch64` or
`qemu-aarch64-static`:

    python3 android/tools/fetch-bionic-runtime.py --output out/android-bionic
    python3 android/tools/run-bionic-test.py --runtime out/android-bionic \
      --qemu /path/to/qemu-aarch64-static out/fex-test/test-fex

QEMU **10.0.13** was validated with this synthetic ABI test and API 30 Bionic.
QEMU 7.2 failed during FEX's high-address allocator setup; do not weaken the
allocator to work around that emulator limitation. On a native ARM64 Linux runner,
omit `--qemu`. On an ARM64 Android device, the test directory can instead be pushed
with `adb` and the binary run with its directory in `LD_LIBRARY_PATH`.

The test exercises integer/XMM arguments, host thunks, a nested guest callback,
code invalidation, the production low-address allocator and the heap regression
described above. QEMU execution,
native ARM64 execution and the launcher emulator test are separate checks; none
runs the original game or validates Vulkan rendering. CI runs the FEX test against
the backend extracted from its own APK. Its Android 15 x86_64 emulator job builds
with `-PlauncherTestAbi=x86_64` for launcher, controls and PKG/source integration;
that APK does not contain the ARM64 game runtime.

### Source bases

The CPU adapter and assembly call bridge come from
[seregasmith/ARM_linux_driveclub-recomp](https://github.com/seregasmith/ARM_linux_driveclub-recomp/tree/562f993c10b022aaf69be0271cd1c3cd287b040c)
at `562f993c10b022aaf69be0271cd1c3cd287b040c`. FEX itself is pinned to
`1c4f04b71a4d33091183fd7e1a2c09e6823d27fd`; `build-fex.sh` applies
`cpu/fex-host-pages.patch` to that revision. The Bionic CMake integration and
`getdents64` compatibility header come from
[erickmendes22/gow3_android_daedalus_fork](https://github.com/erickmendes22/gow3_android_daedalus_fork/tree/b3a7c3d5e916cf8f4622192c0faa9383ebd77c00)
at `b3a7c3d5e916cf8f4622192c0faa9383ebd77c00`; its dependency-build approach also
informs the Android renderer build. The launcher layout and Gradle wrapper follow
[ShadowOfTheColossus-Android-Aot](https://github.com/erickmendes22/ShadowOfTheColossus-Android-Aot).

The PKG/PFS parser is `videobitva/orbis` at
`13f1294a87a53d54f82feec56acbfd03f50667a9`, the revision used by the GoW3 Bionic
PKG build. `android/pkg/vendor/SOURCE.md` records provenance and local hardening;
the MIT and Apache-2.0 license texts are beside it. AdrenoTools is pinned to
`8fae8ce254dfc1344527e05301e43f37dea2df80` and liblinkernsbypass to
`aa3975893d83ef1bc84c321ec60c65fbf1287887`; source changes and BSD-2-Clause licenses
are recorded in `android/third_party/adrenotools/UPSTREAM.md` and that source tree.

The Turnip binary is StevenMXZ's `v39` release asset **721176461**, SHA-256
`bd457b30f195cf93a334b5302c221b1b5cb59c79c485935decf95d560a38c8a2`.
Its Mesa identifier is `27.0.0-devel (git-2648b2059f)`.
`android/third_party/turnip-gen8-v39-patched/{package.json,provenance.json,NOTICE.txt}`
records the exact asset, upstream source links and compatibility caveats; Mesa,
LLVM/libc++ and publisher recipe license notices are included in the same
directory. The release is pinned by binary hash; the publisher's recipe is not
claimed to reproduce it byte for byte.
