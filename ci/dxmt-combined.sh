#!/bin/bash
# Make libdxmt_combined.a, the archive the app links: build/dxmt-ios/build.sh's
# libdxmt_unix.a merged with the LLVM libraries airconv needs (the list in
# dxmt/src/airconv/meson.build, kept in ci/llvm-libs.txt). build.sh only
# refreshes an existing combined archive, so a clean checkout needs this once.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
UNIX="$R/build/dxmt-ios/libdxmt_unix.a"
OUT="$R/build/dxmt-ios/libdxmt_combined.a"
[ -f "$UNIX" ] || { echo "missing $UNIX: run build/dxmt-ios/build.sh first" >&2; exit 1; }
libs=()
while read -r lib; do
    [ -n "$lib" ] && libs+=("$R/toolchains/llvm-ios-build/lib/libLLVM$lib.a")
done < "$R/ci/llvm-libs.txt"
rm -f "$OUT"
xcrun -sdk iphoneos libtool -static -no_warning_for_no_symbols -o "$OUT" "$UNIX" "${libs[@]}"
cp "$OUT" "$R/app/Madeira/libdxmt_combined.a"
echo "libdxmt_combined.a: $(wc -c < "$OUT" | tr -d ' ') bytes"
