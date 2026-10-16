#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "TriggerSpec.h"

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

#define DSB_TRIGGER_RUMBLE_HZ 40 // the vibration a rumble choice plays, in Hz

// The fixed effect for a choice at a strength (0 = default); false for none and
// for rumble (written as the motors move). The values are a starting point to
// tune by feel.
static inline bool DSBTriggerPreset(uint8_t kind, uint8_t strength, uint8_t out[11]) {
    switch (kind) {
    // light, medium and strong are resist at 2, 4 and 6, as the panel, the
    // grammar's comment and per-title-feedback.md say. light used to start at
    // zone 4 and strong at zone 2, so the panel's rewrite of either moved where
    // the resistance began.
    case DSB_TRIGGER_LIGHT:
        DSBTriggerFeedback(3, strength ? strength : 2, out);
        return true;
    case DSB_TRIGGER_MEDIUM:
        DSBTriggerFeedback(3, strength ? strength : 4, out);
        return true;
    case DSB_TRIGGER_STRONG:
        DSBTriggerFeedback(3, strength ? strength : 6, out);
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
