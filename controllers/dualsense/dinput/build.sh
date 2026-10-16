#!/bin/sh
# Build dinput8.dll (x86_64), forwarding to the original kept as dinput8_np.dll.
# It carries no "Wine builtin DLL" mark: wine would then load its own builtin
# from the runner instead of the prefix's copy. Load it as native (dinput8=n,b).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
MINGW_BIN="${MINGW_BIN:-$HOME/.local/cxge/toolchains/llvm-mingw/bin}"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
"$MINGW_BIN/x86_64-w64-mingw32-clang" -shared -O2 -Wall -o "$OUT/dinput8.dll" \
  "$HERE/dinput-ff.c" "$HERE/dinput8.def" -ldxguid -lws2_32 -luser32 -lkernel32 -static-libgcc
echo "built $OUT/dinput8.dll"
