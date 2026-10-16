#pragma once
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdatomic.h>
#include "protocol.h"

// Private state shared by the session loop and its output worker.
// Session configuration (hid/dry/pcmMode) is immutable after worker creation.
// pending/lastSent/history/dirty flags are protected by outputLock.
// Counters, shutdown and legacy motor values are atomic across threads.
typedef struct {
    IOHIDDeviceRef hid;
    bool dry, pcmMode;
    pthread_mutex_t outputLock;
    uint8_t pending[47], lastSent[47];
    bool outputDirty, haveLast, legacyDirty;
    NSMutableArray *sentHistory;
    atomic_bool quitting, workerRunning, workerExited, stalled, deviceRemoved;
    uint16_t product; // USB product id of the paired pad: 0ce6, or 0df2 for the Edge
    atomic_uint reports, errors;
    atomic_ullong writeStartedNS;
    atomic_uchar legacyLeft, legacyRight;
    // --rumble-haptics: the motor values also drive a synthesized waveform on the
    // voice coils while no real haptic audio plays and the link carries PCM.
    bool rumbleHaptics;
    atomic_uchar synthLeft, synthRight;
    // The pad's own trigger positions (0 R2, 1 L2) from its input reports, for
    // trigger effects that wait for the other trigger.
    atomic_uchar padTrigger[2];
    atomic_uint padTriggerReports;
    uint8_t btSequence; // Owned only by the active output worker.
} DSBBrokerState;
extern DSBBrokerState dsb;

// The session loop monitors writes independently of the blocking IOKit call.
bool DSBWriteOutput(const uint8_t *report, unsigned size);
bool DSBOutputWriteTimedOut(void);

// Called on the main run loop before the worker starts; no output is sent.
bool DSBPCMStartInput(void);
bool DSBPCMWorkerIsReady(void);
void *DSBPCMWorker(void *unused);
void DSBPCMNotify(void);
// The link is too slow for PCM (input under 100 Hz) or PCM is waiting to resume.
bool DSBPCMSuspended(void);
void DSBPCMReceive(const DSBPacket *packet);
// Takes its own lock. Callers must not hold outputLock.
NSDictionary *DSBPCMSnapshot(void);

// A Bluetooth input report 0x31 carries L2 and R2 at bytes 6 and 7 (USB report
// 0x01's bytes 5 and 6, one later for the Bluetooth header byte).
static inline void DSBNotePadInput(const uint8_t *report, size_t length) {
    if (length >= 8 && report[0] == 0x31) {
        atomic_store(&dsb.padTrigger[0], report[7]);
        atomic_store(&dsb.padTrigger[1], report[6]);
        atomic_fetch_add(&dsb.padTriggerReports, 1);
    }
}
