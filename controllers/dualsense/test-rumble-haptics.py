"""Offline checks for per-title feedback: rumble on the voice coils and a fixed
trigger resistance. Runs the broker with --dry-run; never opens a controller."""

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


def packet(seq, report, kind, pid=1234, left=0.0, right=0.0):
    return wire.pack(0x31425344, kind, token, pid, seq, time.time() - 978307200, left, right, report)


def neutral(sock, addr, seq, seconds, left, right):
    # The XInput and DirectInput pieces' packet: the two motor values, no report.
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        seq += 1
        sock.sendto(packet(seq, bytes(64), 5, left=left / 255, right=right / 255), addr)
        time.sleep(0.01)
    return seq


def pad(l2, r2):
    # The pad's Bluetooth input report 0x31 with only the two trigger positions
    # set, which a dry-run broker takes in place of the pad's own.
    b = bytearray(64)
    b[0], b[6], b[7] = 0x31, l2, r2
    return bytes(b)


def motors(right, left):
    # A wired output report 0x02 with only the two motor bytes valid, as the
    # xinput piece sends for an XInput title.
    b = bytearray(64)
    b[0], b[1], b[3], b[4] = 2, 3, right, left
    return bytes(b)


def pcm(samples):
    return bytes([30, 1, 0, 0]) + bytes(samples) * 30


def broker(*extra, seconds=12):
    status = root / "rumble-haptics-status.json"
    status.unlink(missing_ok=True)
    args = [str(root / "broker"), "--dry-run", "--token", token.hex(), "--seconds", str(seconds),
            "--status", str(status)] + list(extra)
    log = (root / "rumble-haptics.log").open("w")
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=log, text=True)
    line = p.stdout.readline()
    assert line, "broker did not start: " + " ".join(args)
    return p, json.loads(line)["port"], status, log


def state(status):
    for _ in range(50):
        try:
            return json.loads(status.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(0.05)
    raise AssertionError("no status")


def drive(sock, addr, seq, seconds, report, kind, also=None):
    # `also`: a pad input report sent alongside, from its own source so it does
    # not keep the game's rumble alive.
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        seq += 1
        sock.sendto(packet(seq, report, kind), addr)
        if also:
            seq += 1
            sock.sendto(packet(seq, also, 4, pid=4321), addr)
        time.sleep(0.01)
    return seq


def press(sock, addr, seq, l2, r2, seconds=0.3):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        seq += 1
        sock.sendto(packet(seq, pad(l2, r2), 4, pid=4321), addr)
        time.sleep(0.01)
    return seq


# Rumble plays on the voice coils, and only while there is rumble.
p, port, status, log = broker("--pcm", "--rumble-haptics", "--triggers", "medium", seconds=20)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr = ("127.0.0.1", port)


def controls(s):
    # Control writes (0x31) are every HID report that is not a PCM report.
    return s["hidReports"] - s["pcm"]["reports"]


try:
    time.sleep(1.2)
    s = state(status)
    assert s["rumbleHaptics"] and s["triggerSpec"] == "medium", s
    assert s["pcm"]["synthReports"] == 0 and s["pcm"]["reports"] == 0, s["pcm"]
    # The profile is on both triggers and was sent, before the game writes anything.
    expect = [0x21, 0xF8, 0x03, 0x00, 0xB6, 0x6D, 0x1B, 0, 0, 0, 0]
    assert s["triggers"]["r2"] == expect and s["triggers"]["l2"] == expect, s["triggers"]
    assert any(h["r2"] == expect and h["l2"] == expect for h in s["triggers"]["sentHistory"]), s
    # Values under the motors path's deadband render nothing and cost nothing.
    idle = controls(s)
    seq = drive(sock, addr, 0, 1.0, motors(3, 2), 2)
    time.sleep(1.1)
    s = state(status)
    assert s["pcm"]["synthReports"] == 0 and controls(s) == idle, (idle, s)
    seq = drive(sock, addr, seq, 0.3, motors(0, 0), 2)
    # One rumble episode: rendered on the coils while it lasts, at the cost of
    # opening and closing the stream (two control writes), then nothing more.
    seq = drive(sock, addr, seq, 1.0, motors(200, 120), 2)
    seq = drive(sock, addr, seq, 0.3, motors(0, 0), 2)
    time.sleep(1.1)
    s = state(status)
    synth = s["pcm"]["synthReports"]
    assert synth > 40, s["pcm"]
    assert controls(s) - idle <= 2, (idle, controls(s), s)
    time.sleep(1.1)
    assert state(status)["pcm"]["synthReports"] == synth, "rumble kept rendering after release"
    print("  ok    rumble haptics: rendered while the motors run, nothing under the deadband, "
          "two control writes per episode", flush=True)
    # A game's own haptic audio wins over rendered rumble while it plays.
    end = time.monotonic() + 2.6
    samples = []
    while time.monotonic() < end:
        seq += 1
        sock.sendto(packet(seq, pcm([16, 240]), 3), addr)
        seq += 1
        sock.sendto(packet(seq, motors(255, 255), 2), addr)
        if not samples and time.monotonic() > end - 1.4:
            samples.append(state(status)["pcm"])
        time.sleep(0.01)
    samples.append(state(status)["pcm"])
    first, last = samples
    assert last["reports"] - first["reports"] > 20, samples
    assert last["synthReports"] == first["synthReports"], samples
    seq = drive(sock, addr, seq, 0.3, motors(0, 0), 2)
    print("  ok    rumble haptics: real haptic audio takes precedence", flush=True)
    # The game's own trigger write replaces the profile on the side it writes.
    b = bytearray(64)
    b[0], b[1] = 2, 4
    b[11:22] = bytes([5] + [0] * 10)
    seq += 1
    sock.sendto(packet(seq, bytes(b), 2), addr)
    time.sleep(1.2)
    s = state(status)
    assert s["triggers"]["r2"][0] == 5 and s["triggers"]["l2"] == expect, s["triggers"]
    p.wait(timeout=25)
    assert p.returncode == 0, p.returncode
    # The session releases the profile it set when it ends.
    s = state(status)
    assert s["state"] == "stopped" and s["triggers"]["l2"][0] == 5, s
    print("  ok    trigger profile: sent to both triggers, replaced by the game's own, "
          "released at exit", flush=True)
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    log.close()
    sock.close()

# One effect per trigger: R2 fixed at once; L2 vibrates with the heavy motor,
# in steps, is released when the motor stops, and is the game's once it writes.
p, port, status, log = broker("--pcm", "--triggers", "R2:weapon@8,L2:rumble@4", seconds=12)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr = ("127.0.0.1", port)
try:
    time.sleep(1.2)
    s = state(status)
    assert s["triggers"]["r2"][:4] == [0x25, 0x48, 0, 7], s["triggers"]
    assert s["triggers"]["l2"][0] == 0 and s["triggerRumbleUpdates"] == 0, s
    seq = drive(sock, addr, 0, 0.6, motors(0, 200), 2)
    time.sleep(1.1)
    s = state(status)
    l2 = s["triggers"]["l2"]
    assert l2[0] == 0x26 and l2[9] == 40 and l2[1:3] == [0xFC, 0x03], l2
    # Motor 200 is step 7; at strength 4 that is amplitude 4 (three bits of 3).
    assert (l2[3] >> 6) & 3 == 3 and l2[4] & 1 == 0, l2
    assert s["triggers"]["r2"][0] == 0x25, s["triggers"]
    seq = drive(sock, addr, seq, 0.3, motors(0, 0), 2)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 5 and s["triggerRumbleUpdates"] == 2, s
    # The game writes L2 itself: from then on its rumble no longer moves L2.
    b = bytearray(64)
    b[0], b[1] = 2, 8
    b[22:33] = bytes([0x21, 0xFE, 3, 0, 0x90, 0x44, 0x1A, 0, 0, 0, 0])
    seq += 1
    sock.sendto(packet(seq, bytes(b), 2), addr)
    seq = drive(sock, addr, seq, 0.6, motors(0, 255), 2)
    time.sleep(1.1)
    s = state(status)
    assert s["gameOwnsTriggers"] == [False, True] and s["triggers"]["l2"][0] == 0x21, s
    assert s["triggerRumbleUpdates"] == 2, s
    print("  ok    triggers per side: a fixed effect, rumble that follows the motor in steps, "
          "and the game's own write wins", flush=True)
    p.wait(timeout=15)
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    log.close()
    sock.close()

# A side that waits for the other trigger: R2's weapon only while L2 is pressed,
# L2's rumble only while R2 is, read from the pad's own input reports.
p, port, status, log = broker("--pcm", "--triggers", "R2:weapon@8+L2,L2:rumble@4+R2", seconds=16)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr = ("127.0.0.1", port)
try:
    time.sleep(1.2)
    s = state(status)
    assert s["triggerWaits"] == ["L2", "R2"] and s["triggerWaitChanges"] == 0, s
    assert s["triggers"]["r2"][0] == 0 and s["triggers"]["l2"][0] == 0, s["triggers"]
    # Under the press threshold (40) nothing happens.
    seq = press(sock, addr, 0, 35, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["padTriggers"]["l2"] == 35 and s["padTriggers"]["reports"] > 10, s["padTriggers"]
    assert s["triggers"]["r2"][0] == 0 and s["triggerWaitChanges"] == 0, s
    # Pressed: the weapon goes on R2; above the release threshold (20) it stays.
    seq = press(sock, addr, seq, 200, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["r2"][:4] == [0x25, 0x48, 0, 7] and s["triggerWaitHeld"] == [True, False], s
    seq = press(sock, addr, seq, 25, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["r2"][0] == 0x25 and s["triggerWaitChanges"] == 1, s
    # Released: R2 is free again.
    seq = press(sock, addr, seq, 10, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["r2"][0] == 5 and s["triggerWaitChanges"] == 2, s
    assert any(h["r2"][:4] == [0x25, 0x48, 0, 7] for h in s["triggers"]["sentHistory"]), s
    print("  ok    trigger waits: on while the other trigger is pressed, with hysteresis, "
          "off when released", flush=True)
    # The rumble on L2 waits for R2: the motor alone moves nothing.
    seq = drive(sock, addr, seq, 0.6, motors(0, 200), 2, also=pad(0, 0))
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 0 and s["triggerRumbleUpdates"] == 0, s
    seq = drive(sock, addr, seq, 0.6, motors(0, 200), 2, also=pad(0, 200))
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 0x26 and s["triggerRumbleUpdates"] == 1, s
    # Released, L2 has no effect of its own, so the wait releases it.
    seq = drive(sock, addr, seq, 0.6, motors(0, 200), 2, also=pad(0, 0))
    seq = drive(sock, addr, seq, 0.3, motors(0, 0), 2)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 5 and s["triggerRumbleUpdates"] == 1, s
    assert s["triggerWaitChanges"] == 4, s
    # Once the game writes R2 itself, L2 no longer moves it.
    b = bytearray(64)
    b[0], b[1] = 2, 4
    b[11:22] = bytes([0x21, 0xFE, 3, 0, 0x90, 0x44, 0x1A, 0, 0, 0, 0])
    seq += 1
    sock.sendto(packet(seq, bytes(b), 2), addr)
    seq = press(sock, addr, seq, 200, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["gameOwnsTriggers"][0] and s["triggers"]["r2"][0] == 0x21, s
    assert s["triggerWaitChanges"] == 4, s
    print("  ok    trigger waits: a rumble side follows its wait, and the game's own write wins",
          flush=True)
    p.wait(timeout=20)
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    log.close()
    sock.close()

# Two effects per side: one with the other trigger released, one while it is
# pressed. R2 has light resistance and a weapon while L2 aims; L2 follows the
# heavy motor and resists while R2 is held.
p, port, status, log = broker("--pcm", "--triggers", "R2:resist@2,R2:weapon@8+L2,L2:rumble,L2:resist@6+R2",
                              seconds=16)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr = ("127.0.0.1", port)
try:
    time.sleep(1.2)
    s = state(status)
    base = s["triggers"]["r2"]
    assert base[:3] == [0x21, 0xF8, 0x03] and s["triggers"]["l2"][0] == 0, s["triggers"]
    seq = press(sock, addr, 0, 200, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["r2"][:4] == [0x25, 0x48, 0, 7], s["triggers"]
    seq = press(sock, addr, seq, 0, 0)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["r2"] == base and s["triggerWaitChanges"] == 2, s
    print("  ok    two effects per side: the first while the other trigger is up, the second "
          "while it is pressed", flush=True)
    seq = drive(sock, addr, seq, 0.6, motors(0, 200), 2, also=pad(0, 0))
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 0x26 and s["triggerRumbleUpdates"] == 1, s
    seq = drive(sock, addr, seq, 0.6, motors(0, 200), 2, also=pad(0, 200))
    time.sleep(1.1)
    s = state(status)
    held = s["triggers"]["l2"]
    assert held[:3] == [0x21, 0xF8, 0x03] and held != base and s["triggerRumbleUpdates"] == 1, s
    # Back to the rumble, which starts again from the motor's current value.
    seq = drive(sock, addr, seq, 0.6, motors(0, 200), 2, also=pad(0, 0))
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 0x26 and s["triggerRumbleUpdates"] == 2, s
    seq = drive(sock, addr, seq, 0.3, motors(0, 0), 2)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 5 and s["triggerRumbleUpdates"] == 3, s
    print("  ok    two effects per side: rumble gives way to resistance and comes back", flush=True)
    p.wait(timeout=20)
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    log.close()
    sock.close()

# The neutral motor packet (DSB_MOTORS) drives everything the Sony-shaped
# motor report did: the rendered rumble and a trigger that follows the motor.
p, port, status, log = broker("--pcm", "--rumble-haptics", "--triggers", "L2:rumble", seconds=10)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr = ("127.0.0.1", port)
try:
    time.sleep(1.2)
    seq = neutral(sock, addr, 0, 1.0, 200, 120)
    time.sleep(1.1)
    s = state(status)
    assert s["pcm"]["synthReports"] > 40 and s["outputCommands"] > 50, s
    assert s["triggers"]["l2"][0] == 0x26 and s["triggerRumbleUpdates"] == 1, s["triggers"]
    seq = neutral(sock, addr, seq, 0.3, 0, 0)
    time.sleep(1.1)
    s = state(status)
    synth = s["pcm"]["synthReports"]
    assert s["triggers"]["l2"][0] == 5 and s["triggerRumbleUpdates"] == 2, s
    time.sleep(1.1)
    assert state(status)["pcm"]["synthReports"] == synth, "rumble kept rendering after release"
    # Out of range values are clamped, not refused.
    seq = neutral(sock, addr, seq, 0.3, 400, -50)
    time.sleep(1.1)
    s = state(status)
    assert s["triggers"]["l2"][0] == 0x26 and s["dropped"] == 0, s
    print("  ok    neutral motor packet: renders, moves a rumble trigger and stops like the motor report",
          flush=True)
    p.wait(timeout=15)
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    log.close()
    sock.close()

# A side cannot wait for itself.
r = subprocess.run([str(root / "broker"), "--dry-run", "--pcm", "--token", token.hex(),
                    "--triggers", "R2:weapon+R2"], capture_output=True, timeout=5)
assert r.returncode == 64, r.returncode

# Without the PCM transport there are no voice coils, and the pad's input is
# not read: both options are declined, and a side keeps only its first effect.
p, port, status, log = broker("--rumble-haptics", "--triggers",
                              "R2:weapon+L2,L2:resist,L2:slope+R2", seconds=2)
try:
    time.sleep(1.2)
    s = state(status)
    assert s["rumbleHaptics"] is False and s["triggerWaits"] == [None, None], s
    assert s["triggers"]["r2"][0] == 0 and s["triggers"]["l2"][:3] == [0x21, 0xF8, 0x03], s["triggers"]
    p.wait(timeout=5)
finally:
    if p.poll() is None:
        p.terminate()
    log.close()

# An unknown profile is refused rather than guessed.
r = subprocess.run([str(root / "broker"), "--dry-run", "--pcm", "--token", token.hex(),
                    "--triggers", "R2:max"], capture_output=True, timeout=5)
assert r.returncode == 64, r.returncode
print("==> dualsense per-title feedback: rumble haptics, trigger effects per side, waits, "
      "two effects per side, the neutral motor packet, precedence and refusal")
