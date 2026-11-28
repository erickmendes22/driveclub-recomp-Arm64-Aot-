#!/usr/bin/env bash
# Build the real PS4 PKG/PFS extractor as an NDK-linked static library for JNI.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
pkg_root=$PWD
pkg_toolchain=1.85.0
if ! command -v rustup >/dev/null; then
    echo "Rustup is required for the pinned Rust $pkg_toolchain PKG build." >&2
    exit 1
fi
rustup toolchain install "$pkg_toolchain" --profile minimal
if [[ ${1:-} == --test-only ]]; then
    cargo +"$pkg_toolchain" test --manifest-path android/pkg/Cargo.toml --locked \
        --target-dir "$pkg_root/out/pkg-tests" -p driveclub-pkg-core
    exit
fi
if [[ ${1:-} == --fixtures ]]; then
    cargo +"$pkg_toolchain" run --manifest-path android/pkg/Cargo.toml --locked \
        --target-dir "$pkg_root/out/pkg-tests" -p driveclub-pkg-core \
        --example make-fixtures -- "$pkg_root/out/pkg-fixtures"
    exit
fi
pkg_abi=arm64-v8a
if [[ $# -eq 2 && $1 == --abi ]]; then pkg_abi=$2; shift 2; fi
if [[ $# -ne 0 ]]; then
    echo "Usage: $0 [--test-only | --fixtures | --abi arm64-v8a | --abi x86_64]" >&2
    exit 2
fi
case "$pkg_abi" in
    arm64-v8a) pkg_target=aarch64-linux-android ;;
    x86_64) pkg_target=x86_64-linux-android ;;
    *) echo "Unsupported PKG ABI: $pkg_abi" >&2; exit 2 ;;
esac
pkg_ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z $pkg_ndk || ! -d $pkg_ndk ]]; then
    echo "Set ANDROID_NDK_HOME to the Android NDK installation." >&2
    exit 1
fi
pkg_bin="$pkg_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
[[ -x "$pkg_bin/${pkg_target}30-clang" ]] || {
    echo "The PKG build requires the Linux NDK Android 30 compiler for $pkg_abi." >&2; exit 1;
}
rustup target add --toolchain "$pkg_toolchain" "$pkg_target"
pkg_key=${pkg_target//-/_}
export "CARGO_TARGET_${pkg_key^^}_LINKER=$pkg_bin/${pkg_target}30-clang"
export "CC_$pkg_key=$pkg_bin/${pkg_target}30-clang"
export "AR_$pkg_key=$pkg_bin/llvm-ar"
cargo +"$pkg_toolchain" build --manifest-path android/pkg/Cargo.toml --locked \
    --release --target "$pkg_target" --target-dir "$pkg_root/out/android-pkg" \
    -p driveclub-pkg-core
echo "Built out/android-pkg/$pkg_target/release/libdriveclub_pkg_core.a"
