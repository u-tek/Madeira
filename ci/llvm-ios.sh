#!/bin/bash
# Build the LLVM 15.0.7 static libraries DXMT's airconv links (ci/llvm-libs.txt)
# for iOS arm64, the way build/dxmt-ios/README.md describes the development
# toolchain: a host llvm-tblgen first, then the iOS libraries reusing it.
#
# Outputs (what build/dxmt-ios/build.sh and ci/dxmt-combined.sh read):
#   toolchains/llvm-project/llvm/include   LLVM's source headers
#   toolchains/llvm-ios-build/include      generated headers (llvm-config.h, *.inc)
#   toolchains/llvm-ios-build/lib/*.a      the iOS libraries
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
TC="$R/toolchains"
SRC="$TC/llvm-project"
HOST="$TC/llvm-host-build"
IOS="$TC/llvm-ios-build"
# llvmorg-15.0.7, the commit docs/BUILDING.md records for the development build.
LLVM_SHA=8dfdcc7b7bf66834a761bd8de445840ef68e4d1a
JOBS=$(sysctl -n hw.ncpu)

if [ ! -f "$SRC/llvm/CMakeLists.txt" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/llvm/llvm-project.git
    git -C "$SRC" sparse-checkout set llvm cmake third-party
    git -C "$SRC" fetch -q --depth 1 --filter=blob:none origin "$LLVM_SHA"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

COMMON=(
    -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_PROJECTS=
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
    -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_BINDINGS=OFF
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF
)

echo "=== host llvm-tblgen ==="
cmake -S "$SRC/llvm" -B "$HOST" "${COMMON[@]}"
ninja -C "$HOST" -j"$JOBS" llvm-tblgen

echo "=== iOS libraries ==="
cmake -S "$SRC/llvm" -B "$IOS" "${COMMON[@]}" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DLLVM_HOST_TRIPLE=arm64-apple-ios17.0 -DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios17.0 \
    -DLLVM_TARGET_ARCH=host -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_TABLEGEN="$HOST/bin/llvm-tblgen"
targets=()
while read -r lib; do [ -n "$lib" ] && targets+=("LLVM$lib"); done < "$R/ci/llvm-libs.txt"
ninja -C "$IOS" -j"$JOBS" "${targets[@]}"
ls -la "$IOS/lib/"*.a
