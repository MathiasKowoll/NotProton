#!/bin/sh
# The controller bridge's Windows pieces in a prefix: stage_controller_pieces
# from the run script, against a fake runner and fake prefixes.
set -e
SRC="${1:-$(dirname "$0")/../feats/compat_run.sh}"
[ -f "$SRC" ] || { echo "padstagecheck: $SRC not present, skipped"; exit 0; }

body=""
for f in wine_builtin_dll copy_whole stage_controller_pieces; do
	part=$(sed -n "/^$f() {/,/^}\$/p" "$SRC")
	[ -n "$part" ] || { echo "FAIL: $f not found in $SRC"; exit 1; }
	body="$body
$part"
done
eval "$body"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
log="$work/log"

fails=0
ok() { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s\n         want [%s]\n         got  [%s]\n' "$1" "$2" "$3"; fails=$((fails + 1)); }
is() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi; }
same() { if cmp -s "$2" "$3"; then ok "$1"; else bad "$1" "${2##*/}" "$(head -c 120 "$3" 2>/dev/null | tr -d '\0' | tr '\n' ' ')"; fi; }
gone() { if [ -e "$2" ]; then bad "$1" "no ${2##*/}" "present"; else ok "$1"; fi; }

# A file wine would call its own: "Wine builtin DLL" at offset 0x40.
builtin() {  # builtin <path> <what>
	mkdir -p "$(dirname "$1")"
	{ head -c 64 /dev/zero; printf 'Wine builtin DLL %s\n' "$2"; } > "$1"
}
microsoft() { mkdir -p "$(dirname "$1")"; printf 'MZ Microsoft %s\n' "$2" > "$1"; }

runner="$work/runner"
pieces="$work/pieces"
mkdir -p "$pieces"
for n in mmdevapi xinput1_4 xinput1_3; do
	builtin "$runner/x86_64-windows/$n.dll" "runner x64 $n"
	builtin "$runner/i386-windows/$n.dll" "runner i386 $n"
	printf 'piece %s C:\\notproton-pad.log\n' "$n" > "$pieces/$n.dll"
done

new_prefix() {  # a prefix as wineboot left it: copies of wine's own DLLs
	win="$work/$1/windows"
	rm -rf "$work/$1"
	for n in mmdevapi xinput1_4 xinput1_3; do
		builtin "$win/system32/$n.dll" "prefix x64 $n"
		builtin "$win/syswow64/$n.dll" "prefix i386 $n"
	done
	printf '%s' "$win"
}

stage() {  # stage <windows dir> <machine> <bridge 1|0>
	dsb_native=""
	: > "$log"
	stage_controller_pieces "$1" "$pieces" "$runner" "$2" "$3"
}

echo "== a prefix as wineboot left it, bridge on"
win=$(new_prefix fresh)
cp "$win/system32/xinput1_4.dll" "$work/fresh-own"
stage "$win" 8664 1
is "every piece is loaded as native" "mmdevapi,xinput1_4,xinput1_3" "$dsb_native"
same "the piece takes the DLL's place" "$pieces/xinput1_4.dll" "$win/system32/xinput1_4.dll"
same "_np is the runner's own" "$runner/x86_64-windows/xinput1_4.dll" "$win/system32/xinput1_4_np.dll"
same "the prefix's own is kept" "$work/fresh-own" "$win/system32/xinput1_4_prefix.dll"
stage "$win" 8664 1
same "a second launch keeps the prefix's own" "$work/fresh-own" "$win/system32/xinput1_4_prefix.dll"
is "and says nothing new" "" "$(cat "$log")"

echo "== Microsoft's xinput1_3 in system32 (Devil May Cry 5)"
win=$(new_prefix dmc5)
microsoft "$win/system32/xinput1_3.dll" "x64 xinput1_3"
cp "$win/system32/xinput1_3.dll" "$work/dmc5-ms"
stage "$win" 8664 1
same "_np is the runner's own, not Microsoft's" "$runner/x86_64-windows/xinput1_3.dll" "$win/system32/xinput1_3_np.dll"
same "Microsoft's is kept as the prefix's own" "$work/dmc5-ms" "$win/system32/xinput1_3_prefix.dll"
stage "$win" 8664 0
is "bridge off: nothing is native" "" "$dsb_native"
same "bridge off: Microsoft's is back" "$work/dmc5-ms" "$win/system32/xinput1_3.dll"
gone "and _np is removed" "$win/system32/xinput1_3_np.dll"
gone "and _prefix is removed" "$win/system32/xinput1_3_prefix.dll"

echo "== a prefix staged by the earlier version (_np held the prefix's own)"
win=$(new_prefix earlier)
microsoft "$win/system32/xinput1_3_np.dll" "x64 xinput1_3"
cp "$win/system32/xinput1_3_np.dll" "$work/earlier-own"
cp "$pieces/xinput1_3.dll" "$win/system32/xinput1_3.dll"
stage "$win" 8664 1
same "its _np becomes the prefix's own" "$work/earlier-own" "$win/system32/xinput1_3_prefix.dll"
same "and _np is the runner's own" "$runner/x86_64-windows/xinput1_3.dll" "$win/system32/xinput1_3_np.dll"

echo "== an arm64 prefix (FEX) gets no x86_64 piece"
win=$(new_prefix arm)
cp "$win/system32/mmdevapi.dll" "$work/arm-own"
stage "$win" aa64 1
is "nothing is native" "" "$dsb_native"
same "its own DLL stays" "$work/arm-own" "$win/system32/mmdevapi.dll"
cp "$pieces/mmdevapi.dll" "$win/system32/mmdevapi.dll"
cp "$work/arm-own" "$win/system32/mmdevapi_np.dll"
stage "$win" aa64 1
same "a piece staged there earlier is taken out" "$work/arm-own" "$win/system32/mmdevapi.dll"

echo "== Microsoft's 32-bit xinput1_3 in syswow64"
win=$(new_prefix wow)
microsoft "$win/syswow64/xinput1_3.dll" "i386 xinput1_3"
cp "$win/syswow64/xinput1_3.dll" "$work/wow-ms"
stage "$win" 8664 1
same "the runner's i386 builtin stands in for the session" "$runner/i386-windows/xinput1_3.dll" "$win/syswow64/xinput1_3.dll"
stage "$win" 8664 0
same "and Microsoft's comes back after" "$work/wow-ms" "$win/syswow64/xinput1_3.dll"
gone "with nothing left beside it" "$win/syswow64/xinput1_3_prefix.dll"

echo "== a prefix whose DLL went missing with its original kept"
win=$(new_prefix missing)
cp "$win/system32/xinput1_4.dll" "$work/missing-own"
mv "$win/system32/xinput1_4.dll" "$win/system32/xinput1_4_np.dll"
stage "$win" 8664 0
same "the original comes back" "$work/missing-own" "$win/system32/xinput1_4.dll"

echo "== an older piece"
win=$(new_prefix older)
stage "$win" 8664 1
printf 'older piece C:\\notproton-pad.log\n' > "$win/system32/xinput1_4.dll"
stage "$win" 8664 1
same "is replaced by this one" "$pieces/xinput1_4.dll" "$win/system32/xinput1_4.dll"
leftovers=$(find "$work" -name '*.np-new')
is "no temporary file is left anywhere" "" "$leftovers"

if [ "$fails" -gt 0 ]; then
	echo "==> padstagecheck: $fails failed"
	exit 1
fi
echo "==> padstagecheck: the pieces call the runner's own, the prefix's own comes back, arm64 and syswow64 are left whole"
