"""The suggested launch options file reaches the panel only as the generator
accepts it: the shipped file passes, and each kind of mistake fails the build."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
generator = root / "dylib/embed_options.py"
shipped = root / "dylib/feats/suggested-options.json"


def run(text):
    with tempfile.TemporaryDirectory() as d:
        src, dst = Path(d) / "in.json", Path(d) / "out.h"
        src.write_text(text, encoding="utf-8")
        r = subprocess.run([sys.executable, str(generator), str(src), str(dst), "NP_TEST"],
                           capture_output=True, text=True)
        return r.returncode, dst.read_text() if dst.exists() else None, r.stderr


code, header, err = run(shipped.read_text(encoding="utf-8"))
assert code == 0 and header, err
# The macro holds what the page shows and writes, compact, and nothing the web
# patches use as a marker byte.
body = "".join(line.strip().rstrip("\\").strip()[1:-1] for line in header.splitlines()
               if line.strip().startswith('"'))
literal = body.encode().decode("unicode_escape")
assert json.loads(literal) == {
    appid: {k: v for k, v in entry.items() if k in ("summary", "summary_zh", "options")}
    for appid, entry in json.loads(shipped.read_text(encoding="utf-8")).items()}
assert all(32 <= ord(c) < 127 for c in literal), "non-ASCII or control bytes in the payload"
print("  ok    suggested options: the shipped file generates and round-trips")

good = {"title": "A game", "summary": "What it sets", "measured": "2026-10-16: how",
        "options": {"NOTPROTON_RAW_CONTROLLERS": "1"}}
cases = {
    "not JSON": "{",
    "empty": "{}",
    "a list": "[]",
    "app id not a number": json.dumps({"F1": good}),
    "app id with a leading zero": json.dumps({"0123": good}),
    "app id too large": json.dumps({"4294967296": good}),
    "no measurement": json.dumps({"1": {k: v for k, v in good.items() if k != "measured"}}),
    "an unknown field": json.dumps({"1": dict(good, note="x")}),
    "an empty summary": json.dumps({"1": dict(good, summary=" ")}),
    "a two-line summary": json.dumps({"1": dict(good, summary="a\nb")}),
    "no options": json.dumps({"1": dict(good, options={})}),
    "a bad name": json.dumps({"1": dict(good, options={"A-B": "1"})}),
    "a variable outside the list": json.dumps({"1": dict(good, options={"DYLD_INSERT_LIBRARIES": "x"})}),
    "WINEDLLOVERRIDES": json.dumps({"1": dict(good, options={"WINEDLLOVERRIDES": "dinput8=n"})}),
    "the launcher's DSB_SESSION": json.dumps({"1": dict(good, options={"DSB_SESSION": "1"})}),
    "the launcher's DSB_LIBRARY": json.dumps({"1": dict(good, options={"DSB_LIBRARY": "/tmp/x.dylib"})}),
    "a name holding a path": json.dumps({"1": dict(good, options={"NOTPROTON_SOME_PATH": "/tmp"})}),
    "a value with %": json.dumps({"1": dict(good, options={"DSB_DISABLED": "%command%"})}),
    "an empty value": json.dumps({"1": dict(good, options={"DSB_DISABLED": ""})}),
    "a value needing quotes": json.dumps({"1": dict(good, options={"DSB_DISABLED": "a b"})}),
    "a number value": json.dumps({"1": dict(good, options={"DSB_DISABLED": 1})}),
    "a trigger spec the launcher refuses":
        json.dumps({"1": dict(good, options={"NOTPROTON_PAD_TRIGGERS": "R2:weapon+R2"})}),
    "an unknown trigger effect":
        json.dumps({"1": dict(good, options={"NOTPROTON_PAD_TRIGGERS": "R2:max"})}),
}
for name, text in cases.items():
    code, header, _ = run(text)
    assert code != 0 and header is None, name
cases["a trigger with two effects for the same state"] = json.dumps(
    {"1": dict(good, options={"NOTPROTON_PAD_TRIGGERS": "R2:resist,R2:slope"})})
cases["a trigger spec with an empty item"] = json.dumps(
    {"1": dict(good, options={"NOTPROTON_PAD_TRIGGERS": "R2:resist,"})})
for name in list(cases)[-2:]:
    code, header, _ = run(cases[name])
    assert code != 0 and header is None, name
for spec in ["medium", "weapon@6", "R2:weapon@8+L2,L2:resist@2", "l2:rumble+r2", "R2:off,L2:slope",
             "R2:resist@2,R2:weapon@8+L2,L2:rumble,L2:resist@6+R2"]:
    code, _, err = run(json.dumps({"1": dict(good, options={"NOTPROTON_PAD_TRIGGERS": spec})}))
    assert code == 0, (spec, err)
print("  ok    suggested options: %d kinds of mistake fail the build" % len(cases))
print("==> suggested options: generated as written, refused when the panel could not apply it")
