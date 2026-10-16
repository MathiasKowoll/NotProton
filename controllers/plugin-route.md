# The controller bridge as a NotProton plugin

A study of upstream pull request
[NotProtonNot/NotProton#164](https://github.com/NotProtonNot/NotProton/pull/164),
"initial support for feature plug-ins", against this branch.

The PR's state when studied, on 2026-10-16:

- Open and not a draft, one commit (`f768205`).
- No reviews. The owner said on 2026-10-14 that it would be merged.
- It describes itself as "initial support" for "optional/development features".

Line numbers are from the PR's head.

## What a plugin is in #164

**The descriptor.** A plugin is a plain-text `.conf` file in
`~/.config/notproton/features.d/` (compat_run.sh:1484), one `key = value` per line:

- `key`: the variable its toggle writes into a game's launch options. The run
  script acts only on `NOTPROTON_*`.
- `label`, `description`.
- `section`: only `dev` is accepted (webpatch.c:166, 173).
- `on` / `off`: the values written.
- `depends`: another descriptor's key.
- `requires`: up to four paths; the first that exists is the plugin's dylib.
- `inject = 1`: load that dylib into the game.
- `env = NAME=VALUE`: repeatable; values are literal, with no expansion.

There is no installer, version, signature or app UI. The panel reads the
directory when Steam loads the page; the run script reads it on every launch.

**What it can touch.**

- **Launch:**
  - Static environment variables.
  - One injected dylib per descriptor, appended to `STEAM_DYLD_INSERT_LIBRARIES`,
    which the run script hands to the game as `DYLD_INSERT_LIBRARIES`
    (compat_run.sh:1522-1544).
  - It cannot wrap the command, run a process before or after the game, compute
    a value at launch, or forward a variable it did not list.
- **Prefix:** nothing. An `env = WINEDLLOVERRIDES=...` line would replace the
  run script's own value rather than add to it.
- **Panel:** a checkbox in a "Developer" section, and nothing more:
  - no dropdowns or hint lines;
  - no other section;
  - no row conditional on a core key such as `NOTPROTON_RAW_CONTROLLERS`;
  - no feature on by default.
- **Steam metadata** (store categories): not exposed.

**Measured in the study (/bin/sh).** The name checks before `eval` test only the
first characters:

- a key such as `NOTPROTON_A B` aborts the run script;
- an env name such as `AB$(cmd)` runs `cmd`.

Since a descriptor can inject a dylib anyway, this changes little in practice,
but a descriptor is effectively code.

## This branch against it

| Piece | As #164 stands | What it would need |
|---|---|---|
| `controllers/dualsense/launch` (supervises the session, starts the broker, sets `DSB_*`) | Not possible. Nothing wraps the launch. | A hook the run script execs at its start for the game launch: a generalised version of compat_run.sh:140-142 here. |
| `broker` | Can be shipped, but nothing starts it. | Comes with the hook above. |
| `bridge.dylib` | Can be injected, but cannot be configured: its `DSB_*` values are decided per launch. | The hook, and a way to forward variables to Wine. |
| `mmdevapi.dll`, `xinput1_4.dll`, `xinput1_3.dll` in the prefix | Not possible. | Staging into the prefix after it exists, keeping and restoring the original, and adding `n,b` to the run script's `WINEDLLOVERRIDES` rather than replacing it. |
| `controllers/steam-input/rumble.dylib` | Possible only as an opt-in Developer checkbox per game. | A default-on option, and conditions on core keys (off while `NOTPROTON_RAW_CONTROLLERS=1`). |
| Controller rows in the panel (pad, vibration, trigger editor, hints) | Not possible: checkboxes only. | Select rows with choices, hint lines, a named section, and rows conditional on a core key. The trigger editor writes one value from four dropdowns per side and would need more than that. |
| Suggested options per game | Not possible: nothing is per game. | Better in core. It is not controller-specific, and it matches the owner's stated plan for per-game fixes in the Compatibility tab (comment on #28, 2026-10-13). |
| Store categories (`appinfo` `categories=`) | Not exposed. | Core exporting them, or the plugin reading `appinfo.vdf` itself. |

**Conflict to expect.** #164 takes marker bytes `\027` and `\030` in
`webpatch.c` (lines 32-33). This branch uses the same two bytes for the runner
options and the suggested-options file. Whichever lands second has to move.

## Conclusion

The bridge cannot be a plugin under #164 as written. Two of its pieces fit
already: the injected dylibs, and only the Steam Input adapter with its
configuration.

What it would take to move the bridge (launch, broker, `bridge.dylib`, the
Windows pieces) out of core into a plugin, as additions to the plugin API:

1. A launch hook that wraps the game launch and sees every launch-option variable.
2. A list of variables to forward to Wine.
3. Prefix staging of native DLLs, with their override added to the run script's.

Keeping today's defaults and panel needs more:

4. Default-on options, and conditions on core keys.
5. Select rows, hints and sections in the panel.
6. Store categories made available to plugins.

What should stay in core either way:

- `NOTPROTON_RAW_CONTROLLERS` and its hidraw handling (already upstream);
- the `appinfo` categories line;
- the suggested options per game;
- the launcher's `DYLD_INSERT_LIBRARIES` plumbing.

The multi-controller work (`multi-controller.md`) does not depend on this:
- The backend interface lives inside the broker, which a plugin would ship as
  it is.
- A plugin route would move where the pieces are installed and who starts them,
  not how they work.
