#!/bin/bash
# Configure (first time) and build the ARM64EC FEX module (libarm64ecfex.dll,
# shipped as xtajit64.dll). The options are the WOW64 module's
# (build/fex-wow64/build.sh) with the arm64ec triple: FEX_IOS_HOST_BUILD picks
# the iOS-host link and CRT, and FEX_IOS_HOST turns on the iOS code paths in
# Module.S and the C++ sources.
#
# The *.patch files next to this script are applied to the FEX checkout first
# (once; a patch that is already in the tree is skipped).
set -eu
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PATH="$R/toolchains/llvm-mingw-20260421-ucrt-macos-universal/bin:$PATH"
B="$R/FEX/build-arm64ec"
for p in "$R"/build/fex-arm64ec/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$R/FEX" apply --reverse --check "$p" 2>/dev/null; then
        echo "FEX patch already applied: $(basename "$p")"
    else
        git -C "$R/FEX" apply "$p"
        echo "FEX patch applied: $(basename "$p")"
    fi
done
if [ ! -f "$B/CMakeCache.txt" ]; then
    cmake -S "$R/FEX" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$R/FEX/Data/CMake/toolchain_mingw.cmake" \
        -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
        -DFEX_IOS_HOST_BUILD=ON -DCMAKE_C_FLAGS=-DFEX_IOS_HOST \
        -DCMAKE_CXX_FLAGS=-DFEX_IOS_HOST -DCMAKE_ASM_FLAGS=-DFEX_IOS_HOST \
        -DENABLE_LTO=OFF -DENABLE_ASSERTIONS=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
        -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF -DTUNE_ARCH=generic -DTUNE_CPU=none \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_range-v3=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_unordered_dense=ON -DCMAKE_DISABLE_FIND_PACKAGE_xxhash=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON
    # The DISABLE_FIND_PACKAGE lines keep CMake on FEX's bundled External/ copies:
    # the CI runner's Homebrew fmt is a macOS library, and as an imported target
    # for a Windows build it fails configuration (IMPORTED_IMPLIB not set).
fi
cmake --build "$B" --target arm64ecfex
cp "$B/Bin/libarm64ecfex.dll" "$R/app/Madeira/arm64ec-windows/xtajit64.dll" && ls -l "$R/app/Madeira/arm64ec-windows/xtajit64.dll"
