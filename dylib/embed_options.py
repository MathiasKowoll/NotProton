#!/usr/bin/env python3
# Turns the suggested launch options per game into a C string macro holding the
# JSON as a JavaScript object literal, for the options panel. Refuses a file the
# panel could not apply as written, so a mistake fails the build, not the page.
import json
import re
import sys
from pathlib import Path

FIELDS = {"title", "summary", "summary_zh", "options", "measured"}
REQUIRED = {"title", "summary", "options", "measured"}
# The variables a suggestion may set; the panel holds the same rule for the file
# read at run time (NP_SUGGESTED_NAME_JS in webpatch.c).
NAME = re.compile(r"(?:(?:NOTPROTON|DSB|CX_GRAPHICS|D3DM|DXMT|DXVK|MTL|ROSETTA)_[A-Z0-9_]+|WINE[EM]SYNC)\Z")
# Values the panel writes without quoting (NP_CX_LAUNCH's qv), so the launch
# options read back exactly as suggested.
VALUE = re.compile(r"[\w.,:=/+@%-]+\Z", re.ASCII)
# NOTPROTON_PAD_TRIGGERS as the launcher and the broker accept it
# (controllers/dualsense/Controls.h, DSBParseTriggers).
CHOICE = re.compile(r"(?:off|(?:light|medium|strong|resist|weapon|slope|vibrate|rumble)(?:@[1-8])?)\Z")


def valid_triggers(spec):
    if ":" not in spec:
        return bool(CHOICE.match(spec))
    seen = set()
    for item in spec.split(","):
        side, _, choice = item.partition(":")
        side = side.upper()
        effect, plus, other = choice.partition("+")
        if side not in ("R2", "L2") or not CHOICE.match(effect) or (side, bool(plus)) in seen:
            return False
        if plus and (effect == "off" or other.upper() != ("L2" if side == "R2" else "R2")):
            return False
        seen.add((side, bool(plus)))
    return True


def check(data):
    if not isinstance(data, dict) or not data:
        return "the file must be a non-empty object keyed by Steam app id"
    for appid, entry in data.items():
        if not re.fullmatch(r"[1-9][0-9]{0,9}", appid) or int(appid) >= 1 << 32:
            return f"{appid!r} is not a Steam app id"
        if not isinstance(entry, dict) or set(entry) - FIELDS or REQUIRED - set(entry):
            return f"{appid}: needs {sorted(REQUIRED)} and may add summary_zh, nothing else"
        for field in FIELDS & set(entry) - {"options"}:
            text = entry[field]
            if not isinstance(text, str) or not text.strip() or any(ord(c) < 32 for c in text):
                return f"{appid}: {field} must be one line of text"
        options = entry["options"]
        if not isinstance(options, dict) or not options:
            return f"{appid}: options must name at least one launch option"
        for name, value in options.items():
            if not NAME.match(name):
                return f"{appid}: {name!r} is not a variable a suggestion may set"
            if not isinstance(value, str) or not VALUE.match(value):
                return f"{appid}: {name} needs a non-empty value the panel writes unquoted"
            if name == "NOTPROTON_PAD_TRIGGERS" and not valid_triggers(value):
                return f"{appid}: NOTPROTON_PAD_TRIGGERS={value} is not a spec the launcher accepts"
    return None


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: embed_options.py <input.json> <output.h> <macro>\n")
        return 2
    src, dst, macro = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    try:
        data = json.loads(src.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        sys.stderr.write(f"{src}: {error}\n")
        return 1
    problem = check(data)
    if problem:
        sys.stderr.write(f"{src}: {problem}\n")
        return 1
    # The page needs only what it shows and writes; title and measurement stay
    # in the file. ASCII-only JSON is also a JavaScript literal, and holds none
    # of the control bytes the web patches use as markers.
    shown = {appid: {k: v for k, v in entry.items() if k not in ("title", "measured")}
             for appid, entry in data.items()}
    text = json.dumps(shown, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
    # Each piece is escaped on its own, so a cut never splits an escape.
    chunks = [text[i:i + 80].replace("\\", "\\\\").replace('"', '\\"')
              for i in range(0, len(text), 80)]
    guard = f"NOTPROTON_GENERATED_{macro.upper()}_H"
    body = " \\\n".join(f'    "{c}"' for c in chunks)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(
        f"// Generated from {src.as_posix()} by dylib/embed_options.py. Edit that file.\n"
        f"#ifndef {guard}\n#define {guard}\n\n#define {macro} \\\n{body}\n\n#endif // {guard}\n",
        encoding="ascii")
    return 0


if __name__ == "__main__":
    sys.exit(main())
