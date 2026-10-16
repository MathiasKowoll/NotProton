#include "Controls.h"
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t state[47] = {0}, previous[47] = {0}, out[47];
    state[1] = 4 | 16;
    state[44] = 217;
    state[45] = 255;
    state[43] = 4;
    assert(DSBControlDelta(state, previous, false, out));
    assert(out[1] == 20);
    memcpy(previous, state, 47);
    state[0] = 4;
    state[10] = 0x21;
    state[11] = 0xfe;
    assert(DSBControlDelta(state, previous, true, out));
    assert(out[0] == 4 && out[1] == 0);
    assert(out[43] == 0 && out[44] == 0 && out[45] == 0);
    memcpy(previous, state, 47);
    assert(!DSBControlDelta(state, previous, true, out));
    state[10] = 5;
    state[11] = 0;
    assert(DSBControlDelta(state, previous, true, out));
    assert(out[0] == 4 && out[10] == 5 && out[1] == 0);
    memcpy(previous, state, 47);
    state[44] = 0;
    state[45] = 0;
    assert(DSBControlDelta(state, previous, true, out));
    assert(out[0] == 0 && out[1] == 4);
    memcpy(previous, state, 47);
    state[43] = 2;
    assert(DSBControlDelta(state, previous, true, out));
    assert(out[0] == 0 && out[1] == 16 && out[43] == 2);
    uint8_t release[64];
    DSBTriggerRelease(4, release);
    assert(release[1] == 4 && release[11] == 5 && release[22] == 0);
    DSBTriggerRelease(8, release);
    assert(release[1] == 8 && release[11] == 0 && release[22] == 5);
    DSBTriggerRelease(0, release);
    assert(release[1] == 0 && release[11] == 0 && release[22] == 0);
    uint8_t token[16];
    assert(DSBParseToken("0123456789aBcDeF0123456789aBcDeF", token));
    assert(token[0] == 1 && token[7] == 0xef);
    assert(!DSBParseToken(NULL, token));
    assert(!DSBParseToken("1234", token));
    assert(!DSBParseToken("1z23456789abcdef0123456789abcdef", token));
    assert(!DSBParseToken(" 123456789abcdef0123456789abcdef", token));
    // Feedback resistance: zones from the start position to the end, 3 bits of
    // (strength - 1) each, little endian -- checked against an independent sum.
    uint8_t effect[11];
    DSBTriggerFeedback(3, 4, effect);
    assert(effect[0] == 0x21 && effect[1] == 0xf8 && effect[2] == 0x03);
    uint32_t forces = 0;
    for (unsigned zone = 3; zone < 10; zone++) {
        forces += 3u << (3 * zone);
    }
    assert(effect[3] == (forces & 255) && effect[4] == ((forces >> 8) & 255) &&
           effect[5] == ((forces >> 16) & 255) && effect[6] == (forces >> 24));
    assert(!effect[7] && !effect[8] && !effect[9] && !effect[10]);
    DSBTriggerFeedback(0, 8, effect);
    assert(effect[1] == 0xff && effect[2] == 0x03 && effect[3] == 0xff && effect[6] == 0x3f);
    DSBTriggerFeedback(10, 4, effect);
    assert(effect[0] == 5 && !effect[1]);
    DSBTriggerFeedback(3, 0, effect);
    assert(effect[0] == 5);
    // A slope: each zone keeps its own strength.
    uint8_t slope[10] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    DSBTriggerZones(slope, effect);
    forces = 0;
    for (unsigned zone = 2; zone < 10; zone++) {
        forces += (zone - 2u) << (3 * zone);
    }
    assert(effect[0] == 0x21 && effect[1] == 0xfc && effect[2] == 0x03);
    assert(effect[3] == (forces & 255) && effect[6] == (forces >> 24));
    // Weapon: the start and end zones in the mask, then (strength - 1).
    DSBTriggerWeapon(3, 6, 7, effect);
    assert(effect[0] == 0x25 && effect[1] == 0x48 && effect[2] == 0 && effect[3] == 6);
    assert(!effect[4] && !effect[9]);
    DSBTriggerWeapon(1, 6, 7, effect);
    assert(effect[0] == 5);
    DSBTriggerWeapon(5, 5, 7, effect);
    assert(effect[0] == 5);
    DSBTriggerWeapon(5, 9, 7, effect);
    assert(effect[0] == 5);
    // Vibration: feedback's zone layout, mode 0x26 and the frequency at byte 9.
    DSBTriggerVibration(2, 3, 40, effect);
    assert(effect[0] == 0x26 && effect[1] == 0xfc && effect[2] == 0x03 && effect[9] == 40);
    assert((effect[3] | effect[4] << 8 | effect[5] << 16 | (uint32_t)effect[6] << 24) ==
           ((2u << 6) | (2u << 9) | (2u << 12) | (2u << 15) | (2u << 18) | (2u << 21) | (2u << 24) |
            (2u << 27)));
    DSBTriggerVibration(2, 3, 0, effect);
    assert(effect[0] == 5);
    DSBTriggerVibration(2, 0, 40, effect);
    assert(effect[0] == 5);
    // Choices: one word for both triggers, or one per side, each with an
    // optional strength and, per side, the other trigger it waits for;
    // anything else refused.
    DSBTriggerSpec t;
    assert(DSBParseTriggers("medium", &t) && t.kind[0] == DSB_TRIGGER_MEDIUM &&
           t.kind[1] == DSB_TRIGGER_MEDIUM && !t.strength[0] && !t.strength[1] && !t.when[0] &&
           !t.when[1]);
    assert(DSBParseTriggers("weapon@6", &t) && t.kind[1] == DSB_TRIGGER_WEAPON &&
           t.strength[0] == 6 && t.strength[1] == 6);
    assert(DSBParseTriggers("R2:weapon@8,L2:rumble", &t) && t.kind[0] == DSB_TRIGGER_WEAPON &&
           t.strength[0] == 8 && t.kind[1] == DSB_TRIGGER_RUMBLE && !t.strength[1]);
    assert(DSBParseTriggers("l2:resist@3", &t) && t.kind[0] == DSB_TRIGGER_NONE &&
           t.kind[1] == DSB_TRIGGER_RESIST && t.strength[1] == 3);
    assert(DSBParseTriggers("R2:off,L2:vibrate", &t) && t.kind[0] == DSB_TRIGGER_NONE);
    // R2 waits for L2 (when = 1 + 1), L2 for R2 (when = 1 + 0); the waited
    // effect is the side's second one, beside its own or none.
    assert(DSBParseTriggers("R2:weapon@8+L2", &t) && t.held[0] == DSB_TRIGGER_WEAPON &&
           t.heldStrength[0] == 8 && t.when[0] == 2 && t.kind[0] == DSB_TRIGGER_NONE &&
           t.kind[1] == DSB_TRIGGER_NONE && !t.when[1]);
    assert(DSBParseTriggers("L2:rumble+r2,R2:resist", &t) && t.held[1] == DSB_TRIGGER_RUMBLE &&
           t.when[1] == 1 && t.kind[0] == DSB_TRIGGER_RESIST && !t.when[0]);
    assert(DSBParseTriggers("r2:slope+l2,l2:weapon@3+R2", &t) && t.when[0] == 2 && t.when[1] == 1 &&
           t.heldStrength[1] == 3);
    assert(DSBParseTriggers("R2:resist@2,R2:weapon@8+L2,L2:rumble,L2:resist@6+R2", &t) &&
           t.kind[0] == DSB_TRIGGER_RESIST && t.strength[0] == 2 && t.held[0] == DSB_TRIGGER_WEAPON &&
           t.heldStrength[0] == 8 && t.kind[1] == DSB_TRIGGER_RUMBLE &&
           t.held[1] == DSB_TRIGGER_RESIST && t.heldStrength[1] == 6);
    assert(DSBParseTriggers("R2:weapon+L2,R2:off", &t) && t.kind[0] == DSB_TRIGGER_NONE &&
           t.held[0] == DSB_TRIGGER_WEAPON);
    uint8_t kind, strength;
    DSBParseTriggers("R2:resist@2,R2:weapon@8+L2", &t);
    DSBTriggerActive(&t, 0, false, &kind, &strength);
    assert(kind == DSB_TRIGGER_RESIST && strength == 2);
    DSBTriggerActive(&t, 0, true, &kind, &strength);
    assert(kind == DSB_TRIGGER_WEAPON && strength == 8);
    DSBTriggerActive(&t, 1, true, &kind, &strength);
    assert(kind == DSB_TRIGGER_NONE && !strength);
    const char *bad[] = {"", "max", "R2:max", "R2:", "R2:weapon,", "R2:weapon,R2:light",
                         "R3:light", "R2-light", "Medium", "R2:weapon,L2", "weapon@9",
                         "weapon@0", "weapon@", "weapon@12", "off@3", "R2:off@1", "resist@a",
                         "weapon+L2", "R2:weapon+R2", "R2:weapon+", "R2:weapon+L3",
                         "R2:weapon+L2+L2", "R2:weapon+L2@8", "R2:off+L2", "R2:+L2",
                         "R2:weapon+L2,", "L2:resist+L2", "R2:weapon@8+L",
                         "R2:resist,R2:weapon+L2,R2:slope", "R2:weapon+L2,R2:slope+L2"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!DSBParseTriggers(bad[i], &t));
    }
    assert(!DSBParseTriggers(NULL, &t));
    // Pressed from 40, released under 20, and held in between.
    assert(!DSBTriggerPressed(false, 39) && DSBTriggerPressed(false, 40));
    assert(DSBTriggerPressed(true, 20) && !DSBTriggerPressed(true, 19));
    assert(!DSBTriggerPressed(false, 30) && DSBTriggerPressed(true, 30));
    uint8_t preset[11], expected[11];
    assert(DSBTriggerPreset(DSB_TRIGGER_WEAPON, 0, preset) && preset[0] == 0x25 && preset[3] == 6);
    assert(DSBTriggerPreset(DSB_TRIGGER_WEAPON, 3, preset) && preset[3] == 2);
    assert(DSBTriggerPreset(DSB_TRIGGER_RESIST, 7, preset));
    DSBTriggerFeedback(3, 7, expected);
    assert(!memcmp(preset, expected, 11));
    // The older names are resist at 2, 4 and 6, byte for byte, as the panel writes them.
    {
        const struct { int kind; uint8_t strength; } legacy[] = {
            {DSB_TRIGGER_LIGHT, 2}, {DSB_TRIGGER_MEDIUM, 4}, {DSB_TRIGGER_STRONG, 6}};
        for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
            assert(DSBTriggerPreset(legacy[i].kind, 0, preset));
            assert(DSBTriggerPreset(DSB_TRIGGER_RESIST, legacy[i].strength, expected));
            assert(!memcmp(preset, expected, 11));
        }
    }
    assert(DSBTriggerPreset(DSB_TRIGGER_SLOPE, 0, preset) && preset[0] == 0x21);
    DSBTriggerZones(slope, expected);
    assert(!memcmp(preset, expected, 11)); // the default slope is 1..8, checked above
    assert(DSBTriggerPreset(DSB_TRIGGER_SLOPE, 4, preset) && preset[0] == 0x21);
    // A slope to 4 ends at 4: the last zone's three bits hold 3.
    assert(((preset[6] >> 3) & 7) == 3);
    assert(DSBTriggerPreset(DSB_TRIGGER_VIBRATE, 0, preset) && preset[0] == 0x26 && preset[9] == 25);
    assert(!DSBTriggerPreset(DSB_TRIGGER_RUMBLE, 0, preset) &&
           !DSBTriggerPreset(DSB_TRIGGER_NONE, 0, preset));
    assert(DSBTriggerRumbleAmplitude(0, 4) == 0 && DSBTriggerRumbleAmplitude(8, 0) == 8 &&
           DSBTriggerRumbleAmplitude(8, 4) == 4 && DSBTriggerRumbleAmplitude(1, 1) == 1 &&
           DSBTriggerRumbleAmplitude(1, 8) == 1);
    // Rumble on a trigger: released under the deadband, then eight steps.
    assert(DSBTriggerRumbleStep(0) == 0 && DSBTriggerRumbleStep(3) == 0);
    assert(DSBTriggerRumbleStep(4) == 1 && DSBTriggerRumbleStep(255) == 8);
    for (unsigned v = 4, last = 1; v < 256; v++) {
        unsigned step = DSBTriggerRumbleStep((uint8_t)v);
        assert(step >= last && step <= 8);
        last = step;
    }
    puts("==> dualsense controls: trigger and rumble updates leave LEDs alone; color and release "
         "changes preserved; feedback, slope, weapon and vibration layouts; trigger choices");
}
