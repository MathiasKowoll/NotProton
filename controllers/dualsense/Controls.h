#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// State is retained for comparison, but valid bits are transactions. Replaying
// a stale LED/player color alongside every trigger update fights Steam/macOS.
static inline bool DSBControlDelta(const uint8_t state[47], const uint8_t previous[47], bool known,
                                   uint8_t out[47]) {
    memset(out, 0, 47);
    // Offsets are within the 47-byte common output payload (report ID excluded).
    static const struct {
        unsigned flagByte, validBit, offset, length;
    } fields[] = {
        {0, 4, 10, 11}, // Right adaptive trigger.
        {0, 8, 21, 11}, // Left adaptive trigger.
        {1, 4, 44, 3},  // RGB lightbar.
        {1, 16, 43, 1}, // Player indicator.
        {1, 1, 8, 1},   // Microphone indicator.
    };
    bool changed = false;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        unsigned flag = fields[i].flagByte, bit = fields[i].validBit;
        unsigned offset = fields[i].offset, length = fields[i].length;
        if ((state[flag] & bit) && (!known || !(previous[flag] & bit) ||
                                    memcmp(state + offset, previous + offset, length))) {
            out[flag] |= bit;
            memcpy(out + offset, state + offset, length);
            changed = true;
        }
    }
    return changed;
}

// Release only triggers written by this session; never reset another client's trigger.
static inline void DSBTriggerRelease(uint8_t touched, uint8_t report[64]) {
    memset(report, 0, 64);
    report[0] = 2;
    report[1] = touched & 12;
    if (touched & 4) {
        report[11] = 5;
    }
    if (touched & 8) {
        report[22] = 5;
    }
}

// Trigger effects in the layouts games write through libScePad (the same as
// Nielk1's DualSense trigger effect factory). Each fills the 11-byte effect.
//
// Feedback (0x21): a 10-bit zone mask, then 3 bits of (strength - 1) per zone,
// little endian. `strengths[zone]` 0 leaves a zone free, 1-8 resists. One
// strength from a start zone to the end is a continuous resistance; strengths
// that grow along the travel are a slope.
static inline void DSBTriggerZones(const uint8_t strengths[10], uint8_t out[11]) {
    memset(out, 0, 11);
    uint16_t zones = 0;
    uint32_t forces = 0;
    for (unsigned zone = 0; zone < 10; zone++) {
        if (strengths[zone] >= 1 && strengths[zone] <= 8) {
            zones |= (uint16_t)(1u << zone);
            forces |= (uint32_t)(strengths[zone] - 1) << (3 * zone);
        }
    }
    if (!zones) {
        out[0] = 5; // Off: an empty request never leaves a trigger stiff.
        return;
    }
    out[0] = 0x21;
    out[1] = zones & 0xff;
    out[2] = zones >> 8;
    for (unsigned i = 0; i < 4; i++) {
        out[3 + i] = (uint8_t)(forces >> (8 * i));
    }
}

static inline void DSBTriggerFeedback(uint8_t position, uint8_t strength, uint8_t out[11]) {
    uint8_t strengths[10] = {0};
    for (unsigned zone = position; zone < 10 && strength >= 1 && strength <= 8; zone++) {
        strengths[zone] = strength;
    }
    DSBTriggerZones(strengths, out);
}

// Weapon (0x25): resists from `start` (2-7) and gives way with a click at `end`
// (start+1 to 8); a 10-bit mask of the two zones, then (strength - 1).
static inline void DSBTriggerWeapon(uint8_t start, uint8_t end, uint8_t strength, uint8_t out[11]) {
    memset(out, 0, 11);
    if (start < 2 || start > 7 || end <= start || end > 8 || strength < 1 || strength > 8) {
        out[0] = 5;
        return;
    }
    uint16_t zones = (uint16_t)((1u << start) | (1u << end));
    out[0] = 0x25;
    out[1] = zones & 0xff;
    out[2] = zones >> 8;
    out[3] = strength - 1;
}

// Vibration (0x26): the trigger itself vibrates at `frequency` Hz from zone
// `position` on, with `amplitude` 1-8 (3 bits per zone, as for feedback);
// the frequency sits at byte 9.
static inline void DSBTriggerVibration(uint8_t position, uint8_t amplitude, uint8_t frequency,
                                       uint8_t out[11]) {
    uint8_t amplitudes[10] = {0};
    for (unsigned zone = position; zone < 10 && amplitude >= 1 && amplitude <= 8; zone++) {
        amplitudes[zone] = amplitude;
    }
    DSBTriggerZones(amplitudes, out);
    if (out[0] == 5 || !frequency) {
        memset(out, 0, 11);
        out[0] = 5;
        return;
    }
    out[0] = 0x26;
    out[9] = frequency;
}

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
#define DSB_TRIGGER_RUMBLE_HZ 40
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

// A trigger an effect waits for counts as pressed from 40 of 255 and as
// released under 20, so the effect does not flicker at the edge.
#define DSB_TRIGGER_PRESS 40
#define DSB_TRIGGER_RELEASE 20
static inline bool DSBTriggerPressed(bool pressed, uint8_t value) {
    return value >= (pressed ? DSB_TRIGGER_RELEASE : DSB_TRIGGER_PRESS);
}

// The fixed effect for a choice at a strength (0 = default); false for none and
// for rumble (written as the motors move). The values are a starting point to
// tune by feel.
static inline bool DSBTriggerPreset(uint8_t kind, uint8_t strength, uint8_t out[11]) {
    switch (kind) {
    case DSB_TRIGGER_LIGHT:
        DSBTriggerFeedback(4, strength ? strength : 2, out);
        return true;
    case DSB_TRIGGER_MEDIUM:
        DSBTriggerFeedback(3, strength ? strength : 4, out);
        return true;
    case DSB_TRIGGER_STRONG:
        DSBTriggerFeedback(2, strength ? strength : 6, out);
        return true;
    case DSB_TRIGGER_RESIST:
        DSBTriggerFeedback(3, strength ? strength : 4, out);
        return true;
    case DSB_TRIGGER_WEAPON:
        DSBTriggerWeapon(3, 6, strength ? strength : 7, out);
        return true;
    case DSB_TRIGGER_SLOPE: {
        // From zone 2 up to the chosen strength at the end of travel.
        uint8_t top = strength ? strength : 8, strengths[10] = {0};
        for (unsigned zone = 2; zone < 10; zone++) {
            strengths[zone] = (uint8_t)(1 + (top - 1) * (zone - 2) / 7);
        }
        DSBTriggerZones(strengths, out);
        return true;
    }
    case DSB_TRIGGER_VIBRATE:
        DSBTriggerVibration(2, strength ? strength : 3, 25, out);
        return true;
    default:
        return false;
    }
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
