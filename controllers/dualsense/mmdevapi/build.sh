#!/bin/sh
# Build mmdevapi.dll (x86_64), forwarding to the prefix's original kept as
# mmdevapi_np.dll. No "Wine builtin DLL" mark: with it wine loads its own
# builtin from the runner instead of the prefix's copy. The run script loads
# it as native (n,b).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
MINGW_BIN="${MINGW_BIN:-$HOME/.local/cxge/toolchains/llvm-mingw/bin}"
# llvm-mingw's clang: GNU ld cannot read the .def files' ordinal forwarders.
if [ ! -x "$MINGW_BIN/x86_64-w64-mingw32-clang" ]; then
  echo "error: no llvm-mingw clang in $MINGW_BIN" >&2
  echo "       Set MINGW_BIN to llvm-mingw's bin directory" >&2
  echo "       (https://github.com/mstorsjo/llvm-mingw/releases, 20260616 is the one CI uses)." >&2
  exit 1
fi
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
"$MINGW_BIN/x86_64-w64-mingw32-clang" -shared -O2 -Wall -o "$OUT/mmdevapi.dll" \
  "$HERE/mmdevapi-container.c" "$HERE/mmdevapi.def" \
  -lsetupapi -lole32 -luuid -ladvapi32 -luser32 -lkernel32 -static-libgcc
echo "built $OUT/mmdevapi.dll"
