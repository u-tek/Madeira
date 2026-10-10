#!/bin/bash
# Configure (first time) and build the FEXCore static libraries the app links
# (FEX/build-ios/FEXCore/Source/*.a and External/*). Options mirror the
# development build's CMakeCache.
set -eu
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
B="$R/FEX/build-ios"
if [ ! -f "$B/CMakeCache.txt" ]; then
    cmake -S "$R/FEX" -B "$B" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_FEX_LINUX_TESTS=OFF \
        -DENABLE_FEX_ALLOCATOR=OFF -DENABLE_ASSERTIONS=OFF -DENABLE_CLANG_THUNKS=ON -DENABLE_CCACHE=ON \
        -DTUNE_CPU=cortex-a78
fi
# JemallocLibs (Utils/AllocatorHooks.cpp) is its own target: only FEXCore_shared
# links it, so building FEXCore alone leaves it out, and the app links it.
cmake --build "$B" --target FEXCore FEXCore_Base JemallocLibs
# Every FEX archive app/Madeira.xcodeproj links.
for a in FEXCore/Source/libFEXCore.a FEXCore/Source/libFEXCore_Base.a FEXCore/Source/libJemallocLibs.a \
         External/fmt/libfmt.a External/xxhash/cmake_unofficial/libxxhash.a \
         External/SoftFloat-3e/libsoftfloat_3e.a External/cephes/libcephes_128bit.a; do
    [ -f "$B/$a" ] || { echo "missing $B/$a" >&2; exit 1; }
    ls -l "$B/$a"
done
