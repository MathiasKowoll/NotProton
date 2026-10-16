"""Sources: a source id that speaks again after it expired is a new instance
(Wine reuses process ids) and its restarted sequence is taken; within the
expiry an older sequence is still dropped as a replay. Dry run, no pad."""

import json
import os
import socket
import struct
import subprocess
import time
from pathlib import Path

root = Path(os.environ["DSB_TEST_BUILD"]).resolve()
wire = struct.Struct("<II16sIIdff64s")
token = bytes.fromhex("56" * 16)


def motors(seq, source, level):
    return wire.pack(0x31425344, 5, token, source, seq, time.time() - 978307200, level, level, bytes(64))


def status(path):
    for _ in range(60):
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(0.05)
    raise AssertionError("no status from the broker")


def second_instance(gap):
    """One instance sends motors at zero, falls silent for gap seconds, and a
    second one under the same id counts from 1 again with the motors at 200."""
    path = root / "sources-status.json"
    path.unlink(missing_ok=True)
    broker = subprocess.Popen(
        [str(root / "broker"), "--dry-run", "--token", token.hex(), "--seconds", "8", "--status", str(path)],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    try:
        addr = ("127.0.0.1", json.loads(broker.stdout.readline())["port"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for seq in range(1, 101):
                s.sendto(motors(seq, 0x80000120, 0), addr)
                time.sleep(0.002)
            time.sleep(gap)
            for seq in range(1, 41):
                s.sendto(motors(seq, 0x80000120, 200 / 255), addr)
                time.sleep(0.01)
        time.sleep(1.2)
        return status(path)
    finally:
        broker.terminate()
        broker.wait(timeout=5)


state = second_instance(2.5)
assert state["peakLeft"] > 0.7, state
print("  ok    sources: an id silent past its expiry starts over, and its motors play", flush=True)

state = second_instance(0)
assert state["peakLeft"] == 0, state
assert state["dropped"] >= 40, state
print("  ok    sources: within the expiry a restarted sequence is still dropped as a replay", flush=True)
print("==> dualsense sources: expiry and replay hold")
