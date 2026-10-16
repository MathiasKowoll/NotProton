# Per-title feedback the game does not send

Some titles reach a bridged DualSense only through XInput, so all they send is
two motor values. God of War Ragnarök is one: under Wine it cannot use Sony's
PlayStation PC SDK, so it never loads the libScePad that would drive the
voice coils and adaptive triggers. Two opt-in launch options give such a title
more than the legacy motors. Neither is the game's own authored effect.

| Launch option | Effect |
|---|---|
| `NOTPROTON_PAD_RUMBLE=haptics` | The game's motor values also play on the voice coils. |
| `NOTPROTON_PAD_TRIGGERS=<spec>` | An effect on one trigger or both, with an optional strength. |

Both take effect only in a bridged session (`NOTPROTON_RAW_CONTROLLERS=1`, one
DualSense or DualSense Edge over Bluetooth, or the pad the opt-in GameController
backend takes). The launcher passes them to the broker as
`--rumble-haptics` and `--triggers <spec>`. A spec the launcher does not
understand is not passed on, a spec the broker does not understand ends it
with status 64, and `--rumble-haptics` without the PCM transport is declined
by the broker.

Steam's Compatibility page sets both, with the other pad options, from the
Controllers section once direct access is on (see "The panel" below).

## Rumble on the voice coils

The broker's session loop already folds every source's motor values into one
pair, holding a value while its source is alive. With `--rumble-haptics` it
also publishes that pair, the motor values only, to the PCM writer
(`dsb.synthLeft/synthRight`). The haptic-audio envelope stays out of it: that
audio is already played as PCM, and adding it here would play it twice.

The PCM writer is the only thing that writes to the pad, and its rule is:

- **Real haptic audio wins.** If a game sends haptic audio, the writer plays
  it exactly as before and nothing is synthesized.
- **Otherwise, while a motor value is at least 4 of 255, it renders one.**
  Under 4 nothing is rendered: such a value would only keep a stream of
  near-silence open. (The motors path has a separate rule of the same size: it
  skips a change of less than 4.) Games ramp a rumble out through such values (God of War
  Ragnarök sends 13, 9, 8 ... 1). The heavy
  left motor becomes a 65 Hz tone on the left actuator and the light right
  motor a 160 Hz tone on the right one (`DSBRumbleSynthBlock` in `PCM.h`),
  64 frames per 0x32 report as for real audio. Amplitude ramps across each
  block, so a step in the motor value does not click. The stream stays open
  150 ms past the last nonzero value, the same tail real audio gets, so a
  pulsing rumble does not reopen it every time.
- **On a slow link it does nothing new.** Below 100 Hz input the writer
  suspends PCM, and the motors carry the rumble as they did before. In audio
  mode the motor bytes are already cleared, so the motors and the coils are
  never both driven.

The two tones are a starting point to tune by feel, not a measurement.

While rendered rumble plays, a game's silent haptic stream is not queued
behind it, and the motor values that change meanwhile go out with the switch
back to motors when the stream closes: one episode costs two control writes,
the open and the close.

Cost: while a motor value is above the deadband, the writer streams about 47 PCM reports
per second, as it does for any title with haptic audio, instead of a control
write only when the value moves. The broker's hold keeps a motor value for up
to two seconds after its source's last packet. A title that never sends zero
therefore keeps the stream open while it runs.

## Trigger effects

A spec is one choice for both triggers (`weapon`) or a choice per side
(`R2:weapon@8,L2:slope`, each side at most once). A choice is an effect name
with an optional strength from 1 to 8 (`@n`); `off` takes no strength. Sides
are case-insensitive; effect names are lower case. Each effect is written with the factory
layouts in `Controls.h`, the same modes games write through libScePad:

| Effect | Mode | Shape | Default strength |
|---|---|---|---|
| `resist` | 0x21 feedback | constant from zone 3 to the end of travel | 4 |
| `weapon` | 0x25 weapon | builds from zone 3 and gives way at zone 6 | 7 |
| `slope` | 0x21 feedback | from zone 2, rising per zone up to the strength | 8 |
| `vibrate` | 0x26 vibration | 25 Hz from zone 2 | 3 |
| `rumble` | 0x26 vibration | 40 Hz, amplitude follows the game's heavy (L2) or light (R2) motor | 8 (the top) |
| `off` | none at the start; 0x05 off when a wait ends the side's other effect | the trigger is left to the game | - |

`light`, `medium` and `strong` remain as names for `resist` at strengths 2, 4
and 6, all from zone 3 as `resist` itself.

Each side may also have a second choice, ending in `+L2` or `+R2` to name the
other trigger, which replaces the first while that trigger is pressed.
`R2:resist@2,R2:weapon@8+L2` is light resistance on R2, and a weapon while L2
aims, as a shooter aims with L2 before it fires with R2; `R2:weapon@8+L2` alone
leaves R2 free until L2 is pressed. The broker reads the pad's own trigger
positions from its Bluetooth input reports (0x31, bytes 6 and 7), which only the
PCM transport receives; without it a side keeps only its first choice, with a
line in the log. The other trigger counts as pressed from 40 of 255 and as
released under 20, so the effect does not flicker at the edge. At each change
the side's effect is written again (a released trigger, 0x05, when that state
has none); a `rumble` effect starts from still and follows the motor as usual.
A side has at most one choice of each kind, a second choice cannot be `off`,
and the one-word form has none.

`DSBTriggerFeedback` writes a 10-bit zone mask from the start position to the
end of travel, then 3 bits of `strength - 1` per zone, little endian.

The broker sets the fixed effects when the session starts. `rumble` follows
the motor value in eight steps (`DSBTriggerRumbleStep`), at most one change
every 60 ms, and releases the trigger when the motor stops. A title's own
trigger write replaces the effect on the side it writes, through the same
merge every output goes through, and from then on that side belongs to the
title: rumble no longer moves it (`gameOwnsTriggers` in the status). The
session's cleanup releases any trigger the session set.

The xinput piece follows `XInputEnable`: disabling stops the motors at the
broker (and so the rendered rumble), and enabling sends the last requested
values again, as wine's own xinput does with its device.

A session that ends because the Bluetooth link stalled cannot release the
effects; they then stay on the pad until the next session writes them, or the
pad reconnects.

## The panel

With "Let games read controllers directly" on, the Controllers section of the
Compatibility page shows the bridged pad's options and writes each as a
launch option:

| Row | Launch option |
|---|---|
| Controller the game sees: automatic, Xbox, PlayStation | `NOTPROTON_PAD_XINPUT` unset, `1`, `0` |
| Controller the game sees: Steam Input's controller | `NOTPROTON_PAD_STEAM_INPUT=1` (and the override cleared) |
| Show a DualSense Edge as a standard DualSense | `NOTPROTON_PAD_AS=dualsense` |
| Vibration: motors or haptic actuators | `NOTPROTON_PAD_RUMBLE` unset or `haptics` |
| R2 effect, L2 effect: an effect and "While the other is pressed", each with a strength | one `NOTPROTON_PAD_TRIGGERS` value for both |

With direct access off, the section instead offers "Vibration through Steam
Input", on by default, which writes `NOTPROTON_STEAM_RUMBLE=0` when turned off.

Under each section a line says what the current choice does, and changes with
it: which pad the game sees, where rumble plays, and for a trigger its effect
with the other trigger up and while it is pressed.

Above the other sections, a game listed in `dylib/feats/suggested-options.json`
gets "Suggested for this game": one switch that writes every launch option
measured for it (on) or removes them (off). Each entry names what was measured
and when; `dylib/embed_options.py` refuses at build time an entry the panel
could not write back unquoted, a variable outside NotProton's, the bridge's,
graphics and Rosetta options, or a trigger spec the launcher would refuse.
Steam settings that are not launch options, such as Steam Input, go in the
entry's summary.

The same file in `~/Library/Application Support/notproton/` replaces the
built-in entry for each game it names, and adds games, without a rebuild. The
dylib reads it with the runner options when Steam starts, so a change shows
after a Steam restart. The page uses an entry only if it has a summary and
options the build would accept (the same variables, values the panel writes
unquoted); anything else in the file, or a file that is not JSON, leaves the
built-in suggestions as they are.

The panel reads the older names back as resistance and composes only
canonical specs. A value the launcher would refuse reads as the game's own on
both sides, since the launcher drops it whole.

## Tests

`make dualsense-check` covers:

- **`test-pcm-unit.c`:** silence stays silent; full motors reach full scale;
  each side carries its own tone; block edges stay smooth; a drop ramps out
  within one block.
- **`test-controls.c`:** each layout, checked against an independent sum;
  invalid input turns the trigger off; specs with sides, strengths and waits
  parse, and malformed ones are refused; the press and release thresholds;
  rumble steps and amplitudes.
- **`test-launch.py`:** the options reach the broker only when asked for.
  `haptics` needs PCM, and a spec the launcher does not understand is dropped.
- **`test-rumble-haptics.py`:** run against a dry-run broker.
  - Motor values render on the coils while they last, nothing renders under
    the deadband, an episode costs two control writes, and the rendering ends
    once the motors are released.
  - Real haptic audio takes precedence over rendered rumble.
  - A fixed effect is sent to both triggers, replaced by the game's own
    write, and released when the session ends.
  - Per side: a fixed weapon on R2, and L2 following the heavy motor in steps,
    released when it stops and left alone once the game writes L2.
  - Waits, with input reports played in place of the pad's: R2's effect
    starts only past the press threshold, holds above the release threshold
    and ends under it; a rumble side moves only while its trigger is held;
    a game's write ends the wait. Without PCM a side keeps its first choice.
  - Two choices per side: R2's light resistance gives way to the weapon while
    L2 is pressed and comes back exactly; L2's rumble gives way to resistance
    while R2 is pressed and resumes from the motor's current value.
  - The option is declined without PCM, and an unknown effect is refused.

`make panel-behavior` runs `dylib/tests/panel-behavior/controllers.js`: the
rows appear only with direct access, each writes and reads back its launch
option, every trigger value the panel composes matches the grammar the launcher
and the broker accept (the test checks a JavaScript copy of it), and each
trigger's line follows its choices.

On a pad, check these:

- How it feels compared with the motors.
- The broker status's `pcm.synthReports` and `hidErrors`.
- That an XInput title with the options off behaves exactly as before.
- For a wait: the status's `padTriggers` moving with the pad, and
  `triggerWaitChanges` counting each press and release of the other trigger.
