"""The Bluetooth write budget, in a dry run: with a game's motors moving, the
broker never offers the pad more than a Bluetooth link drains (about 65
reports a second), whatever the per-title options. No pad."""

import json
import math
import os
import socket
import struct
import subprocess
import time
from pathlib import Path

root = Path(os.environ["DSB_TEST_BUILD"]).resolve()
wire = struct.Struct("<II16sIIdff64s")
token = bytes.fromhex("56" * 16)
LINK = 65.0


def motors(seq, left, right):
    return wire.pack(0x31425344, 5, token, 0x80000200, seq, time.time() - 978307200, left, right, bytes(64))


def status(path):
    for _ in range(60):
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(0.05)
    raise AssertionError("no status from the broker")


def offered(extra, seconds=5.0):
    """Reports a second the broker writes while the motors sweep, measured
    between two status snapshots after the start-up writes."""
    path = root / "budget-status.json"
    path.unlink(missing_ok=True)
    broker = subprocess.Popen(
        [str(root / "broker"), "--dry-run", "--pcm", "--token", token.hex(), "--seconds", str(seconds + 2),
         "--status", str(path)] + extra,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    try:
        addr = ("127.0.0.1", json.loads(broker.stdout.readline())["port"])
        start = time.monotonic()
        before = after = None
        seq = 0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            while time.monotonic() - start < seconds:
                t = time.monotonic() - start
                if before is None and t > 1.2:
                    before = status(path)
                if after is None and t > seconds - 0.4:
                    after = status(path)
                seq += 1
                s.sendto(motors(seq, 0.55 + 0.45 * math.sin(9.4 * t), 0.55 + 0.45 * math.sin(14.5 * t + 1)), addr)
                time.sleep(0.016)
        elapsed = after["updated"] - before["updated"]
        return (after["hidReports"] - before["hidReports"]) / elapsed
    finally:
        broker.terminate()
        broker.wait(timeout=5)


for label, extra in [
    ("motors", []),
    ("rumble haptics", ["--rumble-haptics"]),
    ("trigger rumble", ["--triggers", "R2:rumble,L2:rumble"]),
    ("haptics and trigger rumble", ["--rumble-haptics", "--triggers", "R2:rumble,L2:rumble"]),
]:
    rate = offered(extra)
    assert rate <= LINK, (label, rate)
    print(f"  ok    link budget, {label}: {rate:.1f} reports/s", flush=True)
print("==> dualsense link budget: never more than a Bluetooth link drains")
