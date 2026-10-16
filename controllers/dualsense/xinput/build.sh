#!/bin/sh
# Build xinput1_4.dll and xinput1_3.dll (x86_64) for a runner from one source,
# each forwarding to the prefix's original kept as <name>_np.dll. They carry no
# "Wine builtin DLL" mark: wine would then load its own builtin from the runner
# instead of the prefix's copy. The run script loads them as native (n,b).
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
for name in xinput1_4 xinput1_3; do
  "$MINGW_BIN/x86_64-w64-mingw32-clang" -shared -O2 -Wall -o "$OUT/$name.dll" \
    -DORIG_DLL="\"${name}_np.dll\"" "$HERE/xinput-motors.c" "$HERE/$name.def" \
    -lws2_32 -lsetupapi -lhid -luser32 -lkernel32 -static-libgcc
  echo "built $OUT/$name.dll"
done
