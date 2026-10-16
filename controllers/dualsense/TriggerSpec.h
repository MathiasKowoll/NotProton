#pragma once
// Trigger choices as the user writes them (NOTPROTON_PAD_TRIGGERS), whatever
// the pad: the kinds, the grammar, the effect a side has now, the thresholds of
// an effect that waits for the other trigger, and rumble steps. How a pad plays
// a kind is its backend's (the DualSense's layouts are in Controls.h).
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Per-trigger choices for titles that never write trigger effects (XInput
// titles). DSB_TRIGGER_RUMBLE is not a fixed effect: the trigger vibrates with
// the game's own motor value, R2 with the light right motor and L2 with the
// heavy left one. light/medium/strong are resist at strengths 2/4/6.
enum {
    DSB_TRIGGER_NONE,
    DSB_TRIGGER_LIGHT,
    DSB_TRIGGER_MEDIUM,
    DSB_TRIGGER_STRONG,
    DSB_TRIGGER_WEAPON,
    DSB_TRIGGER_SLOPE,
    DSB_TRIGGER_VIBRATE,
    DSB_TRIGGER_RUMBLE,
    DSB_TRIGGER_RESIST,
};
#define DSB_TRIGGER_KINDS 9

static inline int DSBTriggerKind(const char *name, size_t length) {
    static const char *const names[DSB_TRIGGER_KINDS] = {
        "off", "light", "medium", "strong", "weapon", "slope", "vibrate", "rumble", "resist"};
    for (int i = 0; i < DSB_TRIGGER_KINDS; i++) {
        if (strlen(names[i]) == length && !strncmp(name, names[i], length)) {
            return i;
        }
    }
    return -1;
}

// One choice: a kind, optionally "@1".."@8" for its strength (0 = the kind's
// own default). "off" takes no strength.
static inline bool DSBParseTriggerChoice(const char *text, size_t length, uint8_t *kind,
                                         uint8_t *strength) {
    const char *at = memchr(text, '@', length);
    size_t name = at ? (size_t)(at - text) : length;
    int k = DSBTriggerKind(text, name);
    if (k < 0) {
        return false;
    }
    *kind = (uint8_t)k;
    *strength = 0;
    if (at) {
        if (length - name != 2 || at[1] < '1' || at[1] > '8' || k == DSB_TRIGGER_NONE) {
            return false;
        }
        *strength = (uint8_t)(at[1] - '0');
    }
    return true;
}

// What --triggers asks of each trigger; index 0 is R2, 1 is L2. `kind` and
// `strength` are the trigger's effect. When `when` is 1 + the index of the
// other trigger, `held` and `heldStrength` replace it while that trigger is
// pressed: "R2:resist@2,R2:weapon@8+L2" is light resistance, and a weapon while
// L2 is held. A side may have only the second ("R2:weapon+L2").
typedef struct {
    uint8_t kind[2], strength[2], when[2], held[2], heldStrength[2];
} DSBTriggerSpec;

static inline int DSBTriggerSide(const char *name) {
    if ((name[0] != 'R' && name[0] != 'r' && name[0] != 'L' && name[0] != 'l') || name[1] != '2') {
        return -1;
    }
    return name[0] == 'L' || name[0] == 'l' ? 1 : 0;
}

// "medium" or "weapon@6" sets both triggers; "R2:weapon@8,L2:slope" sets each
// (either may be left out, case-insensitive side names), and a side's choice
// ending in "+L2" or "+R2", naming the other trigger, is its effect while that
// trigger is pressed. Each side has at most one of each. False for anything
// else, so an unknown word is refused rather than guessed.
static inline bool DSBParseTriggers(const char *spec, DSBTriggerSpec *out) {
    memset(out, 0, sizeof(*out));
    if (!spec || !*spec) {
        return false;
    }
    if (!strchr(spec, ':')) {
        if (!DSBParseTriggerChoice(spec, strlen(spec), out->kind, out->strength)) {
            return false;
        }
        out->kind[1] = out->kind[0];
        out->strength[1] = out->strength[0];
        return true;
    }
    bool seen[2][2] = {{false, false}, {false, false}};
    const char *at = spec;
    while (*at) {
        const char *comma = strchr(at, ',');
        size_t length = comma ? (size_t)(comma - at) : strlen(at);
        int side = length >= 4 && at[2] == ':' ? DSBTriggerSide(at) : -1;
        if (side < 0) {
            return false;
        }
        const char *plus = memchr(at + 3, '+', length - 3);
        size_t choice = plus ? (size_t)(plus - at) - 3 : length - 3;
        bool waits = plus != NULL;
        uint8_t kind, strength;
        if (seen[side][waits] || !DSBParseTriggerChoice(at + 3, choice, &kind, &strength)) {
            return false;
        }
        if (waits) {
            int other = length - (size_t)(plus - at) == 3 ? DSBTriggerSide(plus + 1) : -1;
            if (other != 1 - side || kind == DSB_TRIGGER_NONE) {
                return false;
            }
            out->when[side] = (uint8_t)(other + 1);
            out->held[side] = kind;
            out->heldStrength[side] = strength;
        } else {
            out->kind[side] = kind;
            out->strength[side] = strength;
        }
        seen[side][waits] = true;
        at += length + (comma ? 1 : 0);
        if (comma && !*at) {
            return false;
        }
    }
    return true;
}

// The effect a side has now: its second one while the other trigger is held.
static inline void DSBTriggerActive(const DSBTriggerSpec *t, int side, bool held, uint8_t *kind,
                                    uint8_t *strength) {
    bool second = t->when[side] && held;
    *kind = second ? t->held[side] : t->kind[side];
    *strength = second ? t->heldStrength[side] : t->strength[side];
}

// The kinds that resist the pull. A pad whose triggers can only vibrate plays
// none of them.
static inline bool DSBTriggerResists(uint8_t kind) {
    return kind != DSB_TRIGGER_NONE && kind != DSB_TRIGGER_VIBRATE && kind != DSB_TRIGGER_RUMBLE;
}

// A trigger an effect waits for counts as pressed from 40 of 255 and as
// released under 20, so the effect does not flicker at the edge.
#define DSB_TRIGGER_PRESS 40
#define DSB_TRIGGER_RELEASE 20
static inline bool DSBTriggerPressed(bool pressed, uint8_t value) {
    return value >= (pressed ? DSB_TRIGGER_RELEASE : DSB_TRIGGER_PRESS);
}

// Rumble on a trigger: the motor value (0-255) in eight steps, so the effect
// changes, and costs a write, only when the step does. Under the motors path's
// deadband (4) the trigger is released.
static inline uint8_t DSBTriggerRumbleStep(uint8_t motor) {
    return motor < 4 ? 0 : (uint8_t)(1 + (motor - 4) * 8 / 252);
}

// The vibration amplitude for a rumble step at a strength (0 = 8, full): the
// step scaled so the strongest motor reaches `strength`, never below 1.
static inline uint8_t DSBTriggerRumbleAmplitude(uint8_t step, uint8_t strength) {
    unsigned top = strength ? strength : 8;
    return step ? (uint8_t)((step * top + 7) / 8) : 0;
}
