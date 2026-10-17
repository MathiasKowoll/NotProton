# DualSense bridge: field tests and findings

What was tried with the controller bridge, what was measured, and what each
finding changed. The bridge is PR #90 (sn0wst0rm) with this branch's changes
on top: the broker, `bridge.dylib` and `AudioFacade.m` in Wine's process, and
the Windows pieces (`mmdevapi.dll`, `xinput1_4.dll`, `xinput1_3.dll`) staged
into each bridged prefix. How the pieces work is in the source and in
[per-title-feedback.md](per-title-feedback.md),
[../multi-controller.md](../multi-controller.md) and
[../plugin-route.md](../plugin-route.md). This file is the record of the
tests.

A result marked **measured** was played on a pad and read from the session's
files. **Built** means compiled and covered by the offline tests only. Dates
are 2026; build numbers are the internal `+pr90.N` suffix of the version.

## Setup

- Mac with an M4 Max, macOS 27, Steam through NotProton.
- Runner: CrossOver Preview 27.0.0.41069 under Rosetta, unless a row says
  otherwise.
- Pad: a DualSense Edge over Bluetooth for almost every run. A standard
  DualSense over Bluetooth was used only where a row says so.
- Direct mode (`NOTPROTON_RAW_CONTROLLERS=1`) with Steam Input **off** for the
  game, unless a row says otherwise.

## What a session leaves to read

| File | What it says |
|---|---|
| `~/Library/Application Support/notproton/controllers/dualsense/sessions/<appid>-status.json` | `state`, `route`, `outputCommands`, `hidReports`, `hidErrors`, `hidStalled`, `pcmBlocks`, `pcm.reports`, `pcm.inputHz`, `pcm.suspended`, `pcm.pauses`, `envelopeUpdates`, `peakLeft`/`peakRight`, trigger counters |
| `sessions/<appid>.log` | The broker's own log: trigger choices, stalls, the last counters |
| `C:\notproton-pad.log` in the prefix | The pieces: which loaded, `pad … container`, `answered`, `bridge Initialize/Start`, `XInputSetState` |
| `<compatdata>/notproton-run.log` | `controllers: bridge route …`, which pieces were staged, the XInput-pad decision |
| `launchers/<appid>/notproton-wine.log` | `virtual haptic stream started`, Wine's errors |

A run **passes** when the pad vibrates and the tester felt it, `hidErrors` is
0, there is no `hidStalled`, the game's sound is normal, and the frame rate is
what it was without the bridge.

## Changes on top of PR #90

| Change | Where | Status |
|---|---|---|
| The DualSense Edge presented as itself: its own 405-byte USB descriptor (read over USB with `ioreg`) and a 64-byte output report | `descriptor.h`, `bridge.m`, `broker.m`, `AudioFacade.m`, `launch.swift` | measured (Mortal Shell II, pr90.3) |
| The launcher starts the game without the bridge instead of exiting 70 when there is no pad, the lock is taken, or the broker fails | `launch.swift` | measured (Spider-Man 2 with the pad asleep) |
| The broker is restarted when it exits 10 (pad gone) or 11 (stalled writer) | `launch.swift` | tested offline; a forced stall on a pad is still owed |
| Motor values are held while their source is alive | `broker.m` | measured (Mortal Shell II) |
| Motors replace PCM while the link is under 100 Hz: the haptic audio's envelope, deadband 4 | `AudioFacade.m`, `broker.m`, `PCMTransport.m` | measured (Spider-Man 2's slow stretches) |
| The virtual audio device accepts the buffer size Preview's winecoreaudio sets before it opens a stream | `AudioFacade.m` | measured (Beast, Spider-Man 2, CONTROL) |
| `mmdevapi.dll`: the bridge's haptic endpoint answers the pad's ContainerId | `mmdevapi/` | measured (Beast, Spider-Man 2, CONTROL) |
| `xinput1_4.dll` / `xinput1_3.dll`: a Sony pad's XInput vibration goes to the broker; with `DSB_XINPUT_PAD=1` the pad is also presented as an XInput pad built from its own HID reports | `xinput/` | measured (Onimusha, DMC5, Crysis 2 and 3, RE2, RE3, MGS4, Savage Planet) |
| The XInput pad is decided by Steam's store category 57 (`NOTPROTON_PAD_XINPUT=1/0` forces) | `helpers/appinfo.swift`, `compat_run.sh` | measured; the rule is wrong for some titles (see findings) |
| `NOTPROTON_PAD_AS=dualsense`: an Edge presented whole as a standard DualSense | `launch.swift`, `bridge.m` | measured (F1 23, God of War) |
| The run script stages the pieces into the prefix for every bridged launch and restores the originals without the bridge | `compat_run.sh` | measured (Mortal Shell II, Resonance, Ghost of Tsushima, RE2, RE3) |
| Steam Input route: the SDL variables reach Wine when direct mode is off | `compat_run.sh` | measured (Wo Long, DMC5; pr90.14) |
| `NOTPROTON_PAD_RUMBLE=haptics` and `NOTPROTON_PAD_TRIGGERS` | `broker.m`, `PCM.h`, `TriggerSpec.h` | measured (Ragnarök, pr90.20 to pr90.24) |
| The Controllers section on Steam's Compatibility page, and `suggested-options.json` | `dylib/feats/webpatch.c`, `suggested-options.json` | the panel was used for the Ragnarök runs; 26 titles suggested |
| A session that starts before its pad (see the last section) | `launch.swift`, `broker.m`, `xinput/`, `mmdevapi/` | built, tested offline |

## Findings

Each finding below was measured, and each one changed the code.

1. **Wine loads the prefix's copy of a builtin DLL, not the runner's.** Pieces
   placed in the runner's `lib/wine` did nothing for a game whose prefix had
   its own `system32` copy. The pieces are staged into the prefix.
2. **A piece marked "Wine builtin DLL" is replaced by the runner's builtin.**
   With the mark at offset 0x40, Wine loaded its own builtin from the runner
   and never the prefix copy (`+loaddll`: `mmdevapi_np` never loaded). The
   pieces carry no mark, and the run script adds
   `mmdevapi,xinput1_4,xinput1_3=n,b` to `WINEDLLOVERRIDES` in bridged
   sessions only.
3. **Audio-haptics titles find the pad's speaker by ContainerId.** Spider-Man
   2, Beast and CONTROL read `DEVPKEY_Device_ContainerId` from every audio
   endpoint. Wine's endpoints have none, and its property store cannot return
   a `VT_CLSID`. The `mmdevapi` piece answers it for the bridge's endpoint
   (identified by its friendly name, `(Bluetooth Bridge)`), with the container
   of the pad's `HID\` node, falling back to its `USB\` parent. A stale
   `BTHENUM\` node is listed as present with another container and must not be
   used. The container changes every session, so it is read again every two
   seconds.
4. **Preview's winecoreaudio sets the buffer size before it opens a stream**
   and fails `Initialize` with `E_FAIL` when that is refused. The virtual
   device accepts 16 to 8192 frames.
5. **The Edge must be presented as an Edge.** Over the standard descriptor,
   Mortal Shell II stopped all output after five start-up commands. The Edge
   declares about 18 more feature reports and a 63-byte output 0x02. Some
   titles know only the standard pad: F1 23's control layer said "No preset
   control scheme" for an Edge, and God of War's libScePad has no 0df2 at all.
   For those, `NOTPROTON_PAD_AS=dualsense` presents the Edge whole as a
   standard pad.
6. **A Bluetooth link drains about 65 output reports a second.** PR #90
   suspends PCM below 100 Hz of input; the envelope fallback then drives the
   motors. Input was seen falling from 125 Hz to 62 to 66 Hz under load, with
   macOS Game Mode's banner shown. The PR's 100 Hz gate is kept as it is.
7. **Steam's store categories are not a reliable Sony-or-XInput rule.**
   Category 57 (PS5 controller) without the XInput pad works for most Sony
   titles, but Wo Long and DYNASTY WARRIORS: ORIGINS read only Steam Input,
   and METAL GEAR SOLID 4 and Revenge of the Savage Planet read only XInput
   despite 57. Those carry a per-title override in `suggested-options.json`.
8. **Steam's virtual pad on macOS is not a HID device.** With Steam Input on,
   Steam's "GamePad-1" (045e:028e) is an `AppleGCSyntheticDevice` published by
   `gamecontrollerd`. Wine's IOHID bus never enumerates it (measured with
   `WINEDEBUG=+hid`, Persona 5 Strikers: the SDL bus started and reported
   nothing). This blocks the hybrid mode (PlayStation glyphs from Steam Input,
   vibration through the bridge) for titles that need Steam's pad.
9. **Some titles drive the pad over Bluetooth themselves.** Resident Evil
   Requiem and PRAGMATA (RE Engine, categories 57 and 58) get adaptive
   triggers through the bridge but never open its haptic endpoint. Without the
   bridge, both vibrate, and PRAGMATA also has trigger resistance. Category 58
   is no rule: Beast and CONTROL have it and need the bridge. PRAGMATA's
   suggestion is `DSB_DISABLED=1`.
10. **A game can ask for XInput once.** Devil May Cry 5 calls
    `XInputGetCapabilities` once at start-up and then waits for a
    device-arrival message. The XInput pad is opened before the first answer
    and reads neutral until the first report. DMC5 imports `xinput1_3` by
    ordinal, so the piece is built under both names.
11. **Steam stores Steam Input per game** in `localconfig.vdf`
    (`UseSteamControllerConfig`: 0 off, 2 on). One run was invalidated because
    it had been left at 2. Check it before blaming a piece.
12. **Check the game's own options first.** Returnal ships with Adaptive
    Triggers off; with it off the game sends L2 off and R2 at minimum, which
    felt like a weak bridge.
13. **Under NotProton, Steam answers "yes" to `IsSteamRunningOnSteamDeck`.**
    God of War Ragnarök then skips the PlayStation PC SDK and never loads
    libScePad. With that answer forced to "no" for a test, the game loads the
    SDK, whose helper processes refuse Wine: `scePsPcInitialize` fails
    (0x8a800111) with SDK 3.50 and 4.00 alike, and the game closes. This is
    the SDK's own refusal and is not worked around. The title gets the
    XInput pad, rumble on the voice coils and fixed trigger effects instead
    (its suggestion).
14. **macOS adds its own audio output for a Bluetooth Edge**, "Speakers
    (DualSense Edge Wireless Controller)", which Wine lists next to the
    bridge's. No game was seen taking it.
15. **A stereo open of the bridge's endpoint is the pad's speaker, not
    haptics.** SILENT HILL: Townfall, Tormented Souls 2 and METAL GEAR SOLID 4
    open it with two channels. The bridge plays only haptics; the speaker
    over Bluetooth needs another report.

## Per-title results

Ordered by how the title reaches the pad. Numbers are from the session's
status file.

### Haptics through the bridge's audio output

| Title | App id | Result |
|---|---|---|
| Beast of Reincarnation | 2001760 | measured, pr90.5: ContainerId answered, `Initialize`/`Start` OK, 5,392 PCM blocks, 282 PCM reports at 129 Hz. Needed Steam Input off: with it on, Steam's virtual pad took XInput slot 0 and the rumble went there |
| Marvel's Spider-Man 2 | 2651280 | measured: 2,590 PCM reports, 0 errors; the envelope fallback covered two slow-link pauses (314 updates, peak 0.89). Again on pr90.27, 2026-10-16 22:41: bridged, 12,774 PCM reports, 0 HID errors, input 125 Hz, no stall |
| CONTROL Resonant | 3669870 | measured: 3,488 PCM reports, no `XInputSetState` at all |
| Resonance: A Plague Tale Legacy | 2713000 | measured, pr90.10: 15,358 blocks, 783 reports. It did not vibrate before finding 2 was fixed |
| SILENT HILL 2 | 2124490 | measured: 24,801 blocks, 1,737 reports, adaptive triggers on both sides (about 1,600 effects each), envelope fallback in the slow stretches |
| Returnal | 1649240 | measured, as an Edge and as a standard pad, once the game's Adaptive Triggers option was on |
| F1 23 | 2108330 | measured with `NOTPROTON_PAD_AS=dualsense`: 4,044 reports, 16,805 blocks, 835 envelope updates, triggers |

### Motors or triggers written to the pad

| Title | App id | Result |
|---|---|---|
| Mortal Shell II | 2584270 | measured, pr90.3 and pr90.8: 17,244 commands, 677 writes, peaks 1.0/1.0, 0 errors |
| Mortal Kombat 1 | 1971870 | measured: motors, fatalities included (406 commands, peaks 0.50). It uses Wwise's motor output, not the audio one |
| Ghost of Tsushima DIRECTOR'S CUT | 2215430 | measured, pr90.9: motors (4,225 commands, left peak 0.59) and trigger effects on both sides; it never read the ContainerId |
| NINJA GAIDEN 4 | 2627260 | measured: 1,803 commands, peaks 0.96/0.96; triggers always off (mode 05) |
| Tormented Souls 2 | 2464280 | measured: motors through SDL's enhanced rumble (250 writes, peaks 0.51), 0 errors |
| Life is Strange: Reunion | 2624870 | measured: motors through SDL, peaks 1.0/1.0. Its adaptive-trigger option was not tried |
| God of War | 1593500 | measured, pr90.23, with `NOTPROTON_PAD_AS=dualsense`: the game drives the pad itself, motors (7,415 commands, peaks 0.98/1.0) and the light bar; no audio haptics |
| SILENT HILL: Townfall | 1636440 | partly: in what was played the game asked for neither motors nor haptics. Two crashes, one in the game's FMOD audio occlusion with none of the bridge's code on the stack; one launch without the bridge did not crash, and neither did a later one with it (ray tracing off) |

### The XInput pad (titles that read only XInput)

| Title | App id | Result |
|---|---|---|
| Onimusha: Way of the Sword | 2638890 | measured, pr90.7: full control, rumble through the broker (117 commands, peaks 0.50/0.80); touchpad click = Back, PS = Guide. Without the bridge it sees no pad |
| Devil May Cry 5 | 601150 | measured: 282 commands, peaks 1.0/1.0, 84 writes, 0 errors (after finding 10). Also works through the Steam Input route since pr90.14 |
| Resident Evil 2 | 883710 | measured: full control, motors (peaks 0.70) |
| Resident Evil 3 | 952060 | measured: full control, motors (peaks 0.70) |
| Crysis 2 Remastered | 2096600 | measured: full control, both motors at full (2,264 commands, 1,247 writes). It ships libScePad and does not use it |
| Crysis 3 Remastered | 2096610 | measured: both motors reached the broker |
| METAL GEAR SOLID 4 | 2492670 | measured with `NOTPROTON_PAD_XINPUT=1`: both motors (140 packets). Without it the game sees no pad: GLFW's mapping database has the standard DualSense, not the Edge |
| Revenge of the Savage Planet | 2787320 | measured with `NOTPROTON_PAD_XINPUT=1`: control and rumble (418 packets), Xbox glyphs. The hybrid mode gave no pad (finding 8) |
| God of War Ragnarök | 2322010 | measured, pr90.24/25, Steam Input on + direct mode + XInput pad + `NOTPROTON_PAD_RUMBLE=haptics` + `NOTPROTON_PAD_TRIGGERS=R2:slope+L2,L2:vibrate`: PlayStation glyphs from Steam Input, 446 commands (peaks 0.77/0.70), 456 of 512 reports as rendered haptics, 28 L2 changes, 0 errors |
| Life is Strange: Double Exposure | 1874000 | inconclusive: control through the XInput pad, but in about 5 minutes the game asked for vibration once, 2.7 s after start (6 packets, peak 0.498) |
| Persona 5 Strikers | 1382330 | not working: through the XInput pad, `XInputSetState` sent only zeros in a fight; through Steam Input, Wine got no pad (finding 8) |

### Steam Input, without direct mode

| Title | App id | Result |
|---|---|---|
| Wo Long: Fallen Dynasty | 1448440 | measured, pr90.14: the game reads Steam Input; its rumble goes through Steam's virtual pad. Only the right side is felt because the game always sends the left motor at 0 (`+xinput,+hid`, 137 writes) |
| DYNASTY WARRIORS: ORIGINS | 2384580 | measured: Steam Input only; Steam sends a light rumble itself, without the bridge |

### Without the bridge, the game drives the pad

| Title | App id | Result |
|---|---|---|
| PRAGMATA | 3357650 | measured with `DSB_DISABLED=1`: vibration and trigger resistance |
| Resident Evil Requiem | 3764200 | measured once without the bridge, because the pad was not connected at launch: it vibrated. With the bridge: triggers only. It has no suggestion yet |

## Pad, mode and link cases

| Case | Status |
|---|---|
| Edge over Bluetooth | the base of every row above |
| Standard DualSense over Bluetooth | owed: Beast, Spider-Man 2, Mortal Shell II |
| Pad on a cable | the launcher steps aside (`original-usb`); not played since the change |
| Two Sony pads connected | the launcher steps aside (`no-single-bluetooth-dualsense`); not played |
| Pad off or waking at launch | stepped aside and the game started (Spider-Man 2, Life is Strange: Double Exposure, Requiem). Replaced by the waiting session below; not played yet |
| Pad lost mid-game | offline only: the broker restarts on exit 10/11 |
| Edge back paddles and Fn | the Edge assigns them itself; nothing broke |
| Switching route between launches of one title | measured: Double Exposure without the bridge then with it, DMC5, MGS4 and Savage Planet across routes; the pieces were restored and staged again each time |
| Direct mode with Steam Input on | the game takes Steam's virtual Xbox pad and the bridge gets nothing (Beast, Onimusha). Undecided: a warning, forwarding, or both |
| Link at 100 Hz or more | fine haptics (`suspended` false, `pcm.reports` growing) |
| Link under 100 Hz | motors from the envelope (`envelopeUpdates` growing); whether it feels right is owed |
| Game Mode on and off | owed: `inputHz` with and without it |
| CrossOver 26.3 and other runners | owed |
| A prefix with pieces, launched without the bridge | measured through the route switches above |
| Long session (30 to 60 min), forced exit, two launches in a row, lights | owed |

## The audit of 2026-10-16

Five reviewers read the branch; the verdict was to keep it, with fixes. The
fixes (twelve commits) and how each was checked:

| Fix | Checked by |
|---|---|
| A PCM write that finds the pad's input stale marks a stall, so the session restarts instead of the writer dying in silence (a PR #90 defect) | new dry-run hook `DSB_TEST_STALE_WRITE` in `test-pcm.py` |
| `_np` is always the runner's own builtin; the prefix's own DLL is kept as `_prefix` and comes back without the bridge. DMC5's prefix had Microsoft's `xinput1_3` as `_np` before. Only x86_64 prefixes are staged; a Microsoft DLL in `syswow64` is swapped for the runner's i386 builtin for the session; copies are atomic | `dylib/tests/padstagecheck.sh` (23 cases) |
| The XInput and `mmdevapi` pieces pin themselves before their threads and patches | compiled only: proving it needs Wine |
| Steam's Exit Game works after a broker that failed to start: TERM is no longer left ignored across `exec` | `test-launch.py`, fails on the old launcher |
| `xinput1_3` titles find the pad's vendor through `xinput1_4_np`; each XInput version has its own source id; the broker resets a source silent for over 2 s, since Wine reuses process ids | `test-sources.py`, fails on the old broker |
| The broker never offers the pad more than the link drains: 62.6 reports/s at most in a dry run, 74.3 before | `test-link-budget.py` |
| Suggestions and runner options cannot set `DSB_SESSION`, `DSB_LIBRARY` or any path | panel tests and `suggested-options-check.py` (25 mistakes) |
| `light`, `medium` and `strong` are resistance 2, 4 and 6 at zone 3, as the panel says | `test-controls.c`, byte by byte |
| CI builds the pieces with llvm-mingw 20260616, sha256 pinned | not run in CI yet: needs a push |
| The run log names the route each session took (`controllers: bridge route …`) | `test-launch.py`; seen live on pr90.27 |
| `mmdevapi` keeps only the ContainerId answer; the DirectInput piece, which was never built, is gone; the notes say what the code does | compiled; every suite passes |

pr90.27 (1.1.9 with these fixes) was built on 2026-10-16 at 22:29 and
installed from the app before 22:40. Its installed files match the app's
payload byte for byte. Spider-Man 2 at
22:41 is the first measured run on it (in the table above); Mortal Kombat 11
at 22:40 logged the Steam Input pad route with its vibration sent to the
bridged pad.

## A session that starts before its pad

Asked for: a pad switched on after the game has started should still be
bridged. Before this change the launcher's probe found no pad, the session
stepped aside (`no-single-bluetooth-dualsense`) and the game ran without the
bridge until it was relaunched.

What makes it possible: `bridge.dylib` decides per call, for every device,
whether it is a Bluetooth DualSense, and Wine's IOHID bus adds a device that
connects later. The virtual audio device depends only on the session's
environment, so it exists from the start. The XInput pad already reopens the
pad every second until it finds one. Only the launcher and the broker needed
a pad at start.

What changed:

- **Launcher.** No pad at all (none over Bluetooth, none on a cable) starts a
  bridged session that waits: `DSB_ROUTE=bridge-waiting`, the broker started
  with `--wait`. Two pads, a pad on a cable, or `NOTPROTON_PAD_WAIT=0` step
  aside as before. The model is unknown, so `DSB_PRODUCT` is left unset,
  unless `NOTPROTON_PAD_AS=dualsense` asks for any Edge to be a standard pad
  (then 0ce6). `DSB_VENDOR` is 054c.
- **Broker.** With `--wait` it binds and announces its port first, drops what
  arrives before the pad, probes once a second, and opens the pad when
  exactly one is connected over Bluetooth. Fixed trigger effects go out then.
  While it waits, the status file says `state: waiting`, with
  `waitingSince` and `droppedWhileWaiting`. It leaves with its launcher, and
  stops cleanly (`state: stopped`) if no pad ever comes.
- **Pad lost mid-game.** On exit 10 or 11 the launcher now restarts the
  broker with `--wait` on the same port, so the pad is taken back whenever it
  returns. Before, the launcher probed for 60 s and then gave up for the rest
  of the session. Three restarts a minute is still the limit.
- **Pieces.** With `DSB_PRODUCT` unset, the XInput pad and the ContainerId
  answer take either model, 0ce6 or 0df2, and the XInput pad reports the one
  it found. Before, the XInput pad looked only for 0ce6 and the ContainerId
  answer took any Sony node.
- **Audio device model.** With no model known the device says 0ce6. Wine
  11.15's winecoreaudio never reads this property.

Tested offline:

- `test-wait.py`: the port is announced before the pad; motors sent while
  waiting are dropped and counted; motors sent after the pad connects play
  (peak over 0.7); with no pad the broker stops cleanly; it leaves when its
  launcher's process goes.
- `test-launch.py`: no pad gives `bridge-waiting` with `--wait`, the bridge's
  library and no model; `NOTPROTON_PAD_AS=dualsense` gives 0ce6; two pads and
  `NOTPROTON_PAD_WAIT=0` step aside; a broker that exits 10 is restarted with
  `--wait` on the same port and with the same credentials.

Still to measure on a pad:

- An audio-haptics title (Spider-Man 2 or Beast) started with the pad off,
  the pad switched on in the menu: does the game ask for the ContainerId
  again when the pad arrives? It asked once at start in every run so far,
  always with the pad present.
- An XInput-pad title (DMC5) started with the pad off: does the device-arrival
  message reach the game, and does it ask `XInputGetCapabilities` again?
- A pad switched off mid-game for more than a minute, then on.
- Resident Evil Requiem: started with the pad off, it used to get no bridge
  and vibrated. Now it gets the bridge when the pad connects. It needs
  `DSB_DISABLED=1`, like PRAGMATA, to keep that.

## Automated tests

| Suite | Covers |
|---|---|
| `make dualsense-check` | the PCM unit tests, the controls byte tests, the HID and output tests, then `test-launch.py`, `test-bridge.py`, `test-pcm.py` (twice, once with deferred timers), `test-sources.py`, `test-wait.py`, `test-link-budget.py`, `test-lifecycle.py`, `test-trigger-routing.py`, `test-rumble-haptics.py` |
| `dylib/tests/padstagecheck.sh` | staging and restoring the pieces in fake prefixes |
| `make steam-rumble-check`, `make settingscheck`, `make runcheck` | the Steam Input adapter, the panel's settings, the run script |
| `dylib/tests/suggested-options-check.py` | `suggested-options.json` and the panel's allowlist |

Two cautions, both observed:

- Run the timing tests with no game in front. On 2026-10-16, with Spider-Man
  2 running, 100 Python sleeps of 2 ms took 5.9 s, and `test-sources.py` ran
  past its broker's 8 s window. It failed the same way with pr90.27's own
  broker.
- `test-bridge.py`'s `audio-test` case `s32 normal` failed once
  (`stoppedAt >= 5`, `audio-test.c:142`) and passed the next two times, with
  no change to `AudioFacade.m`. Its timing margin is still to check.

## Open

- The waiting session on a pad (the list above).
- Steam's virtual pad under Wine (finding 8), which blocks the hybrid mode.
- Several pads in the Steam Input route; `XInputEnable` for the XInput pad;
  the Core Haptics exit codes; hysteresis at the 100 Hz gate.
- The panel's marker bytes against PR #164's; the panel's JavaScript out of
  C macros.
- Store categories Steam leaves unknown in `appinfo`.
