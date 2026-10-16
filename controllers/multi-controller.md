# More controller types for the bridge

A study, written against build `1.1.8-mgvf.71b16ed+options+pr90.26`. Nothing in
this note is implemented. It answers two questions:

- Can the per-title features built for the DualSense also serve other pads, such
  as Nintendo's Switch Pro Controller and Joy-Cons, or Xbox pads?
- Could the vibration work serve any controller?

Each claim about the code names the file and function. Each claim about a device
is marked:

- **measured:** seen on this stack, with the session it came from.
- **documented:** from the vendor's or Apple's interfaces, or from public
  reverse-engineering notes. Not checked here.
- **to measure:** an assumption that needs a session on hardware.

## Summary

Most of what makes the per-title features work is already device-neutral:

- the session supervision;
- the motor model (sources, hold, deadband);
- the per-title options and their grammar;
- the Steam panel and the suggested options;
- the XInput pad presentation as a concept.

The DualSense-only parts are the HID formats and the outputs only a DualSense has.

Vibration already reaches any pad Steam supports on the Steam Input route, through
`controllers/steam-input/rumble.dylib`. Only the DualSense has been measured there.

On the direct route, the motor values are neutral. Only the final write is per
device.

The recommended first step is small and changes no behaviour: give motor values a
device-neutral packet, so the Windows pieces stop encoding them as a Sony output
report. The order after that is:

1. Split the broker into a neutral core and a DualSense backend.
2. Add a GameController backend, using Apple's Core Haptics and controller input.
   It reaches Xbox pads without writing a HID protocol.
3. Add a Switch Pro HID backend, for HD rumble.

## The pieces today

| Piece | Where | What it does | Neutral? |
|---|---|---|---|
| Session supervisor | `dualsense/launch.swift` `main` | Probes the pad, takes `sessions/controller.lock`, makes the token, starts the broker, passes the per-title options, runs the game. | Mostly. The probe result and the bypass reasons name the DualSense; eligibility is "exactly one Bluetooth DualSense, no USB one". |
| Probe and open | `dualsense/DualSense.m` `openController` | Matches `054c:0ce6` and `054c:0df2` and counts the Bluetooth and USB ones. | No. |
| Packet envelope | `dualsense/protocol.h` `DSBPacket` | Magic, token, pid, sequence, timestamp, two floats, 64 bytes. | Yes. |
| Motor packets | `xinput/xinput-motors.c` `send_motors_locked`, `dinput/dinput-ff.c` | Motor values sent as `DSB_MOTORS`: the two values in the envelope's floats, no report (step 1). | Yes. |
| Source model | `broker.m` `receivePackets`, `rumbleHeld`, `updateEffects` | Folds every source's motors and haptic-audio envelope, and holds a value while its source is alive. | Yes: the core since step 2. The deadband (`moved`) is the DualSense backend's. |
| Control writer | `DualSense.m` `outputWorker`, `Controls.h` `DSBControlDelta` | Bluetooth report `0x31` with CRC32 (seed `0xa2`); triggers, lightbar, player and mic LEDs. | No. |
| PCM writer | `dualsense/PCMTransport.m` | Haptic reports `0x32` at about 47/s, input-rate check, suspends under 100 Hz. | No. |
| Rumble synthesis | `dualsense/PCM.h` `DSBRumbleSynthBlock` | Turns the two motor values into a 65 Hz and a 160 Hz tone. | Yes, as a signal. Its packing into `0x32` is Sony's. |
| Trigger effects | `Controls.h` `DSBTriggerFeedback`, `DSBTriggerWeapon`, `DSBTriggerVibration`, `DSBTriggerPreset` | DualSense trigger modes `0x21`, `0x25`, `0x26`, `0x05`. | No. |
| Trigger spec | `TriggerSpec.h` `DSBParseTriggers`, `DSBTriggerActive`, `DSBTriggerPressed` | `NOTPROTON_PAD_TRIGGERS` grammar, two choices per side, the press/release thresholds, rumble steps. | Yes. `R2` and `L2` can be read as "right" and "left trigger". |
| Trigger waits | `broker.m` `updateTriggerWaits`, `Session.h` `DSBNoteTriggerPositions` | Switch a side's effect on the other trigger's position. | Yes. The DualSense backend reads the positions from Bluetooth report `0x31`, bytes 6 and 7 (`BrokerInternal.h` `DSBNotePadInput`). |
| Core Haptics fallback | `DualSense.m` `prepare`, `setLevel`, `renewPlayers` | Without PCM, the motors drive `GCController.haptics` engines on the left and right handle localities. | Nearly. Only the `GCDualSenseGamepad` class check and `nativePad` are DualSense. |
| HID presentation | `dualsense/bridge.m` | Inside Wine: presents the Bluetooth pad as a USB one (descriptor, report sizes, transport, the Edge as a standard pad); answers feature `08/02`; turns input `0x31` into `0x01`; sends output `0x02` to the broker. | No. Games speak Sony HID to a DualSense. |
| Haptic audio endpoint | `dualsense/AudioFacade.m`, `mmdevapi/mmdevapi-container.c` | A 4-channel endpoint for haptic audio, and its ContainerId. | No. |
| XInput pad | `xinput-motors.c` `init_pad`, `open_pad`, `pad_reader` | Builds an XInput pad from the pad's HID input reports. Forwards vibration only for vendor `054c`. | The presentation is neutral. Input decoding and the vendor gate are Sony. |
| Steam Input rumble | `steam-input/rumble.cpp`, `SteamSession.h` | Forwards the rumble of Steam's virtual pad to `ISteamController::TriggerVibration`. | Yes. Steam renders it on whatever pad it holds. |
| Run script | `dylib/feats/compat_run.sh` `plan_hidden_controllers`, the XInput-pad decision | Writes `Hidraw=0` for the pads Steam handles. The Nintendo ids `057e/2006`, `2007`, `2009` get it even with Steam Input off. Picks the XInput pad from store category 57. | Partly. Category 57 is "PS5 controller support", a Sony signal. |
| Panel and suggestions | `dylib/feats/webpatch.c` `NP_CX_CONTROLLER_*`, `suggested-options.json` | Writes the launch options. | Mostly. Labels name the DualSense; `NOTPROTON_PAD_AS` is Sony-only. |

## What each pad could offer

| Pad | Motors | Fine haptics | Trigger effects | Lights | Today on this stack |
|---|---|---|---|---|---|
| DualSense, DualSense Edge | 2 | Voice coils: PCM `0x32` on Bluetooth, or haptic audio. **Measured.** | Resistance, weapon, vibration modes. **Measured.** | Lightbar, player LEDs. **Measured.** | Full bridge. |
| DualShock 4 | 2 | None | None | Lightbar | Steam Input route only. Bluetooth output report `0x11` with CRC: **documented** (Linux `hid-playstation` and `hid-sony`). |
| Switch Pro Controller (`057e:2009`) | Two linear actuators ("HD rumble") | Each takes a high-band and a low-band frequency and amplitude per update: **documented** (community reverse-engineering of output reports `0x10` and `0x01`). | None: ZL and ZR are digital. | Player LEDs, home LED, through subcommands: **documented**. | Wine is kept off its hidraw (`Hidraw=0`) unless direct access is on. The run script's comment says this keeps most games' controller support working. Rumble on either route: **to measure**. |
| Joy-Con (`057e:2006`, `2007`) | One actuator each, as the Pro | As the Pro | None | Player LEDs | As the Pro, and a pair is two devices. **To measure.** |
| Xbox Series, Xbox One (Bluetooth) | 2 | None | Impulse triggers (rumble motors in RT and LT). XInput cannot drive them; Windows.Gaming.Input and GameInput can: **documented** (Microsoft). | Guide button only | Wine maps it to XInput. Rumble on both routes and trigger haptics through `GCHapticsLocalityLeftTrigger`/`RightTrigger` over Bluetooth: **to measure**. Apple's API has trigger localities: **documented**. |
| Generic HID pads | 0 to 2 | None | None | None | Wine's own mapping. |

What follows from the table:

- **Motors** are the common ground, and the source model already produces them for
  every pad.
- **Fine haptics** exist on the DualSense and the Switch pads. The two-band tone in
  `PCM.h` maps to HD rumble's encoding: a frequency and an amplitude per band, sent
  as small updates rather than an audio stream. That may suit HD rumble better than
  the DualSense's PCM. To measure.
- **Trigger effects:**
  - Force effects (`resist`, `weapon`, `slope`) are DualSense-only.
  - On Xbox pads, `vibrate` and `rumble` could become impulse-trigger rumble: the
    game's motors on the triggers, which no Windows game can ask for through XInput.
  - The Switch pads have none.
- **Trigger waits** need trigger positions. Any backend that reads the pad's input
  can supply them: as an analog value, or as 0 and 255 for a digital ZL/ZR.

## A neutral core and per-device backends

### The core

These parts work the same for every pad and stay where they are or move into a
core file:

- The supervisor: `launch.swift`, without the DualSense eligibility rule.
- The packet envelope.
- The source model and the deadband.
- The per-title options:
  - the rumble-haptics decision;
  - the trigger spec, the active choice and the waits;
  - the release at exit.
- The status file.
- The Steam Input rumble adapter.
- The panel and the suggestions.

### The backend interface

Built in step 2. A backend is the only writer to its pad. Its interface is
`DSBBackend` in `controllers/dualsense/Session.h`, and the DualSense's is
`DSBDualSenseBackend` in `DualSense.m`:

| Member | What the core asks of it |
|---|---|
| `argument` | A flag only this backend knows (the DualSense's `--pcm`, `--trigger-reset-grace-ms`). |
| `open` | Probe (one JSON line about the pads present) or open the pad; an exit status on failure. |
| `abilities` | What the opened pad can do: `DSB_CAN_HAPTICS` (rumble on fine haptics), `DSB_CAN_AUDIO` (haptic audio as a stream), `DSB_CAN_READ_TRIGGERS` (trigger positions for waits). |
| `realTime` | Whether the run loop wakes on the socket every 2 ms (PCM) or turns every 10 ms. |
| `start` | Start writing once the socket exists. |
| `accepts`, `receive` | Packets only this pad understands: a game's own output report, haptic audio, and in a dry run a pad input report. `receive` says which trigger sides a game wrote and which motor values it set. |
| `update` | Every turn: play the motor values and haptic-audio envelope the core folded (`DSBMotorState`). |
| `trigger` | An abstract trigger effect: a `TriggerSpec.h` kind and strength, a rumble amplitude, or a release. |
| `failed`, `status`, `counters` | The writer stalled or the pad went away; its part of the status file; its write counters. |
| `stop` | Release what the session set, stop writing, close the pad; the exit status and final state. |

What the core does with it (`broker.m`):

- It keeps the packets and their sources, the motor values folded from them, the
  trigger spec, waits, rumble triggers and game ownership, and the status file.
- It declines an option the pad's `abilities` cannot honour, with one log line:
  rumble haptics without `DSB_CAN_HAPTICS`, waits without
  `DSB_CAN_READ_TRIGGERS`.
- A backend reports trigger positions with `DSBNoteTriggerPositions`, and the
  core reads them for waits.
- The grammar and the rumble steps are in `TriggerSpec.h`. The DualSense's trigger
  modes stay in `Controls.h`.

Not yet done:

- Dry runs still go through the DualSense backend, so the tests read Sony bytes.
  A recording backend would let the core's tests check calls instead.
- Lights are left out of the interface. On the DualSense, games and Steam write
  them, and the bridge passes them through.

### On the wire

Done in step 1. `DSB_MOTORS` carries the two motor values in the envelope's
`left` and `right` floats and nothing in `report`:

- `xinput-motors.c` and `dinput-ff.c` send it.
- `DSB_OUTPUT` stays for what it really is: a game's own DualSense output report,
  which only the DualSense backend reads.

### Who may write to the pad

The DualSense work showed that a second writer on Bluetooth costs frames: the
link drains about 65 output reports a second, and every extra writer adds load
to it. That is why the broker is the pad's only writer. A bridged session for
another pad needs the same rule.

- **Switch pads:** Wine must not also own the pad. `bridge.m` can keep it out of
  Wine's enumeration in a bridged session, and the XInput piece then presents the
  pad from the backend's decoded input, as it already does for a DualSense in a
  title without category 57.
- **Xbox pads:** Wine maps them to XInput itself. Wine's own rumble write and a
  backend's would both reach the pad, so the backend should take over the motors
  and Wine's XInput vibration should go to the broker. The XInput piece's vendor
  gate becomes "the bridged pad" (see below) rather than `054c`.

### Naming

- The launcher passes the backend to the pieces as `DSB_DEVICE`, plus `DSB_VENDOR`
  and `DSB_PRODUCT` (`DSB_PRODUCT` exists today). The pieces' vendor gates read
  them instead of `054c`.
- The user-facing options keep their names:
  - `NOTPROTON_PAD_RUMBLE=haptics` means "the pad's fine haptics": voice coils on
    a DualSense, HD rumble on a Switch pad.
  - `NOTPROTON_PAD_TRIGGERS` keeps `R2` and `L2` as "right" and "left trigger".
  - `NOTPROTON_PAD_AS` stays Sony-only.
- The panel cannot know which pad is connected, because options are per game. It
  offers every effect, and its hint says which pads have force effects. The broker
  declines the rest per pad, with one log line.
- Store category 57 decides the XInput pad only for Sony pads. Other pads are
  always presented as XInput, since games do not speak their HID protocol.

## What the vibration work can do for any controller

**Steam Input route.** `rumble.dylib` already forwards a game's XInput rumble on
Steam's virtual pad to Steam, and Steam renders it on the physical pad: motors on
an Xbox pad, HD rumble on a Switch pad.
- Measured with a DualSense: INVERSUS Deluxe (see the adapter's README) and
  Wo Long: Fallen Dynasty.
- Other pads: to measure. This is the cheapest test of the question: a Switch Pro
  or an Xbox pad, Steam Input on, direct access off, one XInput title.

**Direct route.**
- Motor values from XInput, DirectInput and a game's own reports are already
  folded per session, whatever the pad.
- With a backend per pad, each per-title option means the same thing everywhere:
  - rumble on the fine haptics;
  - effects on the triggers that can take them;
  - the second effect while the other trigger is held.

**Core Haptics.** Apple's `GCController.haptics` drives the haptics of any pad
macOS supports, and the broker's non-PCM path already uses it (`prepare`,
`setLevel`). A GameController backend would read input through `GCController` as
well, which is enough for waits and for an XInput pad.
- Rumble through Core Haptics on Xbox and Switch pads: to measure.
- Latency compared with a direct HID write: to measure.
- Trigger localities on Xbox pads over Bluetooth: to measure.

## Steps

Every step keeps the DualSense working. Each runs `make dualsense-check` and
`make panel-behavior`, plus the tests named.

0. **Commit what exists** in reviewable pieces (the user's call), so the refactor
   has a base.
1. **A neutral motor packet.** *Done.* Add `DSB_MOTORS`; the XInput and DirectInput pieces
   send it, and the broker accepts it beside the motor-only `0x02`.
   - Tests: `test-rumble-haptics.py` sends both forms and gets the same synth and
     motor results.
   - No change on a pad.
2. **Split the broker.** *Done, except the recording backend.* Move the source model, the per-title options and the
   waits into a core file. Move `outputWorker`, `PCMTransport.m`, the trigger
   layouts and `openController` behind a DualSense `DSBBackend`.
   - This is a refactor: every existing test must pass unchanged.
   - Dry runs gain a `dry` backend that records calls, so the core's tests stop
     reading Sony bytes.
3. **Choose the backend.** The probe reports `DSB_DEVICE`; the launcher passes it,
   with vendor and product, to the broker and the pieces, whose vendor gates read
   it.
   - Tests: `test-launch.py` cases for the environment.
   - The DualSense is still the only backend.
4. **GameController backend.** Core Haptics on the handles (and the triggers where
   offered), input from `GCController`.
   - It covers Xbox pads first.
   - Measure on hardware:
     - rumble felt, latency, and frame cost;
     - impulse triggers over Bluetooth;
     - that Wine's own write is gone.
5. **Switch Pro HID backend.**
   - HD rumble from the two-band synth, sent as rumble updates.
   - Input report `0x30` (enabled by subcommand `0x03`) for waits and for the XInput
     pad.
   - The pad kept out of Wine's enumeration in a bridged session.
   - Needs a Switch Pro on the bench. Joy-Cons follow as a pair of devices.
6. **DualShock 4 backend**, if wanted: motors and lightbar on Bluetooth report
   `0x11`, in the Sony family.

## To measure before step 4

- The Steam Input route with a Switch Pro and an Xbox pad: does `rumble.dylib`'s
  forwarding vibrate them? (Zero code.)
- With direct access off, does Wine's own mapping vibrate a Switch Pro through
  XInput? The `Hidraw=0` keys send it through Wine's SDL path.
- `GCController.haptics.supportedLocalities` for each pad on this Mac.
- The output report rate a Switch Pro accepts over Bluetooth, the counterpart of
  the DualSense's measured drain of about 65 reports a second.
