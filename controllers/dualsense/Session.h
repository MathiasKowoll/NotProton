#pragma once
// What the broker's device-neutral core shares with the backend that drives one
// kind of pad, and the interface that backend implements. The core keeps the
// session: the packets and the sources they come from, the motor values folded
// from them, the per-title options and the status file. A backend owns the pad:
// opening it, every write to it, and what only that pad has.
// controllers/multi-controller.md has the reasoning and the plan.
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

typedef struct {
    bool dry;             // --dry-run: no pad is opened or written
    atomic_bool quitting; // the session loop ends at its next turn
    bool rumbleHaptics;   // --rumble-haptics, kept only when the backend can play it
    // The pad's own trigger positions (0 right, 1 left), 0 to 255, for effects
    // that wait for the other trigger. A backend that reads the pad's input
    // reports them with DSBNoteTriggerPositions.
    atomic_uchar padTrigger[2];
    atomic_uint padTriggerReports;
} DSBSession;
extern DSBSession session;

static inline void DSBNoteTriggerPositions(uint8_t right, uint8_t left) {
    atomic_store(&session.padTrigger[0], right);
    atomic_store(&session.padTrigger[1], left);
    atomic_fetch_add(&session.padTriggerReports, 1);
}

// One session's counters, owned by the core's run loop. A backend fills left,
// right and the peaks with what it played, and envelopeUpdates when haptic
// audio drove the motors.
typedef struct {
    uint64_t accepted, dropped, levels, outputs, envelopeUpdates;
    float left, right, peakLeft, peakRight;
} SessionMetrics;

// What the core folded from every source on one turn, 0 to 1, left being the
// heavy motor: the motor values held for live sources, and the haptic-audio
// envelope of the sources that sent one in the last 150 ms.
typedef struct {
    float left, right, envelopeLeft, envelopeRight;
} DSBMotorState;

// What a game's own output report asked of the pad, as its backend read it.
typedef struct {
    unsigned triggers; // the sides it wrote: bit 0 right, bit 1 left
    bool motors;       // it set motor values
    float left, right; // those values, 0 to 1
} DSBGameOutput;

// What an opened pad can do. The core declines an option it cannot honour.
enum {
    DSB_CAN_HAPTICS = 1,       // play motor values on fine haptics (--rumble-haptics)
    DSB_CAN_AUDIO = 2,         // take haptic audio as a stream (DSB_PCM)
    DSB_CAN_READ_TRIGGERS = 4, // report the pad's trigger positions
    DSB_CAN_TRIGGER_FORCE = 8, // resist the pull (resist, weapon, slope)
    DSB_CAN_TRIGGER_RUMBLE = 16, // vibrate a trigger (vibrate, rumble)
};

// What a backend's probe found: its pads on each transport, and whether a
// session can take one (exactly one over Bluetooth and none on a cable).
typedef struct {
    unsigned bluetooth, usb;
    bool eligible;
    uint16_t vendor, product; // the pad a session would take, 0 when none
} DSBProbe;

typedef struct {
    const char *name;
    // A probe asks it only when it is named (--backend): it is still to be
    // measured on a pad.
    bool onlyWhenNamed;
    // A flag only this backend knows, at argv[*i]; it moves *i past a value.
    // False for a flag it does not know, or a value it refuses.
    bool (*argument)(int argc, char **argv, int *i);
    // Count this backend's pads without opening one. 0, else an exit status.
    int (*probe)(DSBProbe *found);
    // Open the pad a session takes. 0 on success, else the broker's exit status.
    int (*open)(void);
    // What the opened pad can do (DSB_CAN_*).
    unsigned (*abilities)(void);
    // The pad's writer needs packets as they arrive: the run loop then wakes on
    // the socket and turns every 2 ms, rather than every 10 ms.
    bool (*realTime)(void);
    // Start writing once the session socket exists. False ends the session.
    bool (*start)(void);
    // Packets only this backend understands: a game's own output report
    // (DSB_OUTPUT), haptic audio (DSB_PCM) and, in a dry run, a pad input report
    // (DSB_INPUT). `accepts` runs before a packet is attributed to a source;
    // `receive` fills `out` for DSB_OUTPUT and gets NULL otherwise.
    bool (*accepts)(const DSBPacket *packet);
    void (*receive)(const DSBPacket *packet, DSBGameOutput *out);
    // Every turn of the session loop: play the folded motors. False ends it.
    bool (*update)(double now, const DSBMotorState *motors, float gain, SessionMetrics *metrics);
    // A trigger effect (TriggerSpec.h kinds), strength 1 to 8 or 0 for the
    // default. For DSB_TRIGGER_RUMBLE the strength is the vibration's amplitude,
    // 0 still. DSB_TRIGGER_NONE releases the trigger.
    void (*trigger)(int side, uint8_t kind, uint8_t strength);
    // The writer stalled or the pad went away: the session ends.
    bool (*failed)(void);
    // Its part of the status file, and its write counters for the last log line.
    void (*status)(NSMutableDictionary *status);
    void (*counters)(unsigned *writes, unsigned *errors);
    // Release what the session set, stop writing and close the pad. Returns the
    // broker's exit status and names the final state for the status file.
    int (*stop)(NSString **state);
} DSBBackend;

// The backends a broker can run, in the order a probe asks them.
extern const DSBBackend DSBDualSenseBackend;
extern const DSBBackend DSBGameControllerBackend;
