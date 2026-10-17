#!/bin/sh
# Builds the runner's x86_64 mmdevapi.dll (see container.c) with mingw-w64.
# Wine loads a DLL from a runner's lib/wine only if it carries the builtin
# signature at offset 0x40: "Wine builtin DLL" and the zeros after it, as
# Wine's own DLLs have it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
mkdir -p "$OUT"
"$CC" -shared -O2 -Wall -Wextra -o "$OUT/mmdevapi.dll" \
  "$HERE/container.c" "$HERE/mmdevapi.def" \
  -lsetupapi -lole32 -ladvapi32 -static-libgcc -s
{ printf 'Wine builtin DLL'; head -c 16 /dev/zero; } | dd of="$OUT/mmdevapi.dll" bs=1 seek=64 conv=notrunc 2>/dev/null
echo "built $OUT/mmdevapi.dll"
