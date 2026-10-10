#!/bin/bash
# Configure the two Wine build trees the native build scripts read, which the
# repository has no recipe for, and build what is needed from them:
#   wine/build-macos    host tree with the aarch64 PE side (docs/WOW64.md).
#                       build/ntdll-unix, win32u-unix and wineserver read its
#                       config.h and generated headers; its aarch64-windows
#                       plugplay/svchost/winedevice are bundled (gitignored).
#   wine/build-arm64ec  the flags build/wine-pe/build-ntdll.sh uses; dwrite's
#                       unixlib reads its widl-generated headers.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$R/toolchains/llvm-mingw-20260421-ucrt-macos-universal/bin:$PATH"
JOBS=$(sysctl -n hw.ncpu)

configure() {
    local dir="$R/wine/$1"; shift
    if [ ! -f "$dir/config.status" ]; then
        mkdir -p "$dir"
        (cd "$dir" && ../configure "$@")
    fi
}

configure build-macos --enable-archs=aarch64 --without-x --without-vulkan --disable-tests --enable-winegstreamer
configure build-arm64ec --enable-archs=arm64ec --without-x --disable-tests --enable-winegstreamer

make -C "$R/wine/build-macos" -j"$JOBS" include/all
make -C "$R/wine/build-arm64ec" -j"$JOBS" include/all
make -C "$R/wine/build-macos" -j"$JOBS" programs/plugplay/all programs/svchost/all programs/winedevice/all
