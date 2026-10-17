"""A session that waits for its pad, in a dry run: the broker announces its
port before there is a pad, drops what arrives meanwhile, takes the pad when
it connects (DSB_TEST_PAD_AFTER), and stops cleanly if none ever does. No pad."""

import json
import os
import re
import socket
import struct
import subprocess
import time
from pathlib import Path

root = Path(os.environ["DSB_TEST_BUILD"]).resolve()
wire = struct.Struct("<II16sIIdff64s")
token = bytes.fromhex("56" * 16)


def motors(seq, level):
    return wire.pack(0x31425344, 5, token, 0x80000300, seq, time.time() - 978307200, level, level, bytes(64))


def status(path):
    for _ in range(60):
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(0.05)
    raise AssertionError("no status from the broker")


def broker(path, extra, env=None):
    path.unlink(missing_ok=True)
    process = subprocess.Popen(
        [str(root / "broker"), "--dry-run", "--wait", "--token", token.hex(), "--status", str(path)] + extra,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=dict(os.environ, **(env or {})),
    )
    ready = json.loads(process.stdout.readline())
    assert ready["ready"] and ready["waiting"], ready
    return process, ("127.0.0.1", ready["port"])


# The pad connects 3 s in: motors sent in the first second are dropped,
# motors sent after it connects play.
path = root / "wait-status.json"
process, addr = broker(path, ["--seconds", "4"], {"DSB_TEST_PAD_AFTER": "3"})
try:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        start = time.monotonic()
        seq = 0
        while time.monotonic() - start < 1:
            seq += 1
            s.sendto(motors(seq, 1.0), addr)
            time.sleep(0.02)
        waiting = status(path)
        assert waiting["state"] == "waiting" and waiting["port"] == addr[1], waiting
        deadline = time.monotonic() + 5
        while status(path)["state"] != "running" and time.monotonic() < deadline:
            time.sleep(0.1)
        running = status(path)
        assert running["state"] == "running", running
        assert running["peakLeft"] == 0 and running["accepted"] == 0, running
        for seq in range(seq + 1, seq + 31):
            s.sendto(motors(seq, 200 / 255), addr)
            time.sleep(0.02)
        time.sleep(1.2)
        played = status(path)
    assert played["peakLeft"] > 0.7 and played["port"] == addr[1], played
    print("  ok    wait: the port comes first, motors before the pad are dropped, after it they play",
          flush=True)
finally:
    process.terminate()
    _, err = process.communicate(timeout=5)
# The count comes from the broker's own line: the status file is written once
# a second and may still be the first one, from before anything arrived.
dropped = re.search(r"; (\d+) packets dropped meanwhile", err)
assert "No pad yet" in err and dropped and int(dropped.group(1)) > 0, err

# No pad ever: the broker waits until the session ends, and stops cleanly.
process, addr = broker(path, ["--seconds", "1.5"])
out, err = process.communicate(timeout=6)
assert process.returncode == 0, (process.returncode, err)
assert status(path)["state"] == "stopped", status(path)
print("  ok    wait: a pad that never comes ends the session cleanly", flush=True)

# The launcher's parent goes away while it waits: the broker goes too.
parent = subprocess.Popen(["sleep", "1"])
process, addr = broker(path, ["--parent", str(parent.pid)])
parent.wait()
process.wait(timeout=5)
assert process.returncode == 0, process.returncode
print("  ok    wait: a broker waiting for its pad leaves with its launcher", flush=True)
print("==> dualsense wait: a session can start before its pad")
