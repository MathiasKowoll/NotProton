#!/bin/sh
# The runner's mmdevapi.dll must export what Wine's does, under the same
# ordinals, and carry the builtin signature.
set -eu
DLL="$1"
OBJDUMP="${MINGW_OBJDUMP:-x86_64-w64-mingw32-objdump}"
fail() { echo "FAIL: $*"; exit 1; }
# Wine compares the signature with its terminating zero.
[ "$(dd if="$DLL" bs=1 skip=64 count=32 2>/dev/null | od -An -c | tr -d ' \n')" = 'WinebuiltinDLL\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' ] \
	|| fail "no builtin signature"
exports=$("$OBJDUMP" -p "$DLL" | sed -n '/^Export Address Table -- Ordinal Base/,/^$/p')
echo "$exports" | grep -q 'Ordinal Base 2' || fail "ordinal base is not 2"
[ "$(echo "$exports" | grep -c '+base\[')" = 24 ] || fail "not 24 exports"
! echo "$exports" | grep -q Forwarder || fail "a forwarder Wine would resolve by name"
ordinal=17
for name in ActivateAudioInterfaceAsync DllCanUnloadNow DllGetClassObject DllRegisterServer \
	DllUnregisterServer DriverProc auxMessage midMessage modMessage; do
	"$OBJDUMP" -p "$DLL" | grep -q "+base\[  $ordinal\].* $name\$" || fail "$name is not ordinal $ordinal"
	ordinal=$((ordinal + 1))
done
echo "==> mmdevapicheck: Wine's exports and ordinals, builtin signature"
