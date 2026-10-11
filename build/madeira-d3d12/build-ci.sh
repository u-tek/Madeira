#!/bin/bash
# CI build of madeira_d3d12.dll (ARM64EC), replacing the prebuilt copies the app
# ships as arm64ec-windows/madeira_d3d12.dll and arm64ec-windows/d3d12.dll (the
# same binary under both names, see build-pe.sh). d3d12core.dll stays prebuilt.
#
# build-pe.sh links against DXMT's PE-side import library, which CI does not
# build. The DLL only imports winemetal.dll functions by name, so the import
# library is made here from the exports of the winemetal.dll the app ships.
set -eu
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TC="$R/toolchains/llvm-mingw-20260421-ucrt-macos-universal"
SRC="$R/madeira-d3d12/src/pe"
OUT="$R/build/madeira-d3d12/out-ci"
APP="$R/app/Madeira/arm64ec-windows"
mkdir -p "$OUT"

python3 "$SRC/gen_vtables.py" "$TC/generic-w64-mingw32/include/d3d12.h" "$SRC/madeira_d3d12_stubs.h" >/dev/null

{
    echo "LIBRARY winemetal.dll"
    echo "EXPORTS"
    "$TC/bin/llvm-readobj" --coff-exports "$APP/winemetal.dll" | sed -n 's/^ *Name: //p'
} > "$OUT/winemetal.def"
echo "winemetal.dll exports: $(($(wc -l < "$OUT/winemetal.def") - 2))"
"$TC/bin/llvm-dlltool" -m arm64ec -d "$OUT/winemetal.def" -l "$OUT/libwinemetal.a"

echo "prebuilt madeira_d3d12.dll: $(wc -c < "$APP/madeira_d3d12.dll") bytes"
"$TC/bin/arm64ec-w64-mingw32-clang" -shared -O2 -Wall \
    -o "$OUT/madeira_d3d12.dll" "$SRC/madeira_d3d12.c" "$SRC/d3d12.def" \
    -I"$SRC" -I"$R/madeira-d3d12/src" -I"$R/dxmt/src/winemetal" \
    -L"$OUT" -lwinemetal -luuid -lole32
cp "$OUT/madeira_d3d12.dll" "$APP/madeira_d3d12.dll"
cp "$OUT/madeira_d3d12.dll" "$APP/d3d12.dll"
echo "built madeira_d3d12.dll: $(wc -c < "$APP/madeira_d3d12.dll") bytes (also shipped as d3d12.dll)"
