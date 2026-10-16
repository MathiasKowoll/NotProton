// The DualSense backend: everything the broker does that only a DualSense or a
// DualSense Edge has. It opens the pad, writes to it (Bluetooth report 0x31 with
// its CRC, or the PCM transport in PCMTransport.m), merges a game's own output
// reports, plays motor values on the motors, the voice coils or Core Haptics,
// and turns trigger choices into the pad's trigger modes (Controls.h). The
// device-neutral core is broker.m; Session.h is the interface between them.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#import <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <zlib.h>
#include <mach/mach.h>
#include "BrokerInternal.h"
#include "HapticChannel.h"
#include "Calibration.h"
#include "Controls.h"
#include "PCM.h"

// --pcm selects the bounded direct PCM transport; without it Core Haptics
// plays the motors.
static DSBHapticChannel handles[2] = {{.level = -1}, {.level = -1}};
static GCDualSenseGamepad *nativePad;
static IOHIDManagerRef manager;
static pthread_t worker;
// Optional grace period for interleaved active-trigger and reset commands.
// Off commands are delayed only briefly after an active feedback command.
// A genuine release is applied within resetGrace; effect bytes stay unchanged.
static double resetGrace, lastActive[2], releaseAt[2];
static uint8_t releaseEffect[2][11];
static unsigned suppressedResets[2];
// Trigger effects received per side (0 = R2 at offset 10, 1 = L2 at offset 21).
static unsigned triggerUpdates[2];
static uint64_t ledCommands, playerCommands;
static uint32_t ledSource;
static uint8_t ledRGB[3], playerLED;
DSBBrokerState dsb = {.outputLock = PTHREAD_MUTEX_INITIALIZER};

static uint64_t writeTimeoutNS(void) {
    return dsb.pcmMode ? 80000000 : 250000000;
}

static bool markWriteTimeout(uint64_t started) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (!started || now - started <= writeTimeoutNS() ||
        atomic_load(&dsb.writeStartedNS) != started) {
        return false;
    }
    if (!atomic_exchange(&dsb.stalled, true)) {
        atomic_fetch_add(&dsb.errors, 1);
        fprintf(stderr, "HID output timed out after %.1f ms\n", (now - started) / 1e6);
    }
    return true;
}

bool DSBOutputWriteTimedOut(void) {
    return markWriteTimeout(atomic_load(&dsb.writeStartedNS));
}

bool DSBWriteOutput(const uint8_t *report, unsigned size) {
    if (atomic_load(&dsb.stalled)) {
        return false;
    }
    uint64_t started = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    atomic_store(&dsb.writeStartedNS, started);
    IOReturn result =
        session.dry ? kIOReturnSuccess
                : IOHIDDeviceSetReport(dsb.hid, kIOHIDReportTypeOutput, report[0], report, size);
    markWriteTimeout(started);
    atomic_store(&dsb.writeStartedNS, 0);
    atomic_fetch_add(&dsb.reports, 1);
    if (result) {
        atomic_fetch_add(&dsb.errors, 1);
        atomic_store(&dsb.stalled, true);
        fprintf(stderr, "HID output failed: error=0x%x report=0x%02x size=%u\n", result, report[0],
                size);
    }
    return !atomic_load(&dsb.stalled);
}

static void queueOutput(const uint8_t *usb) {
    const uint8_t *src = usb + 1;
    pthread_mutex_lock(&dsb.outputLock);
    // Merge independent valid fields, optionally defer transient trigger resets.
    double now = CFAbsoluteTimeGetCurrent();
    for (int side = 0; side < 2; side++) {
        unsigned bit = 4u << side, offset = 10 + side * 11;
        if (!(src[0] & bit)) {
            continue;
        }
        triggerUpdates[side]++;
        const uint8_t *effect = src + offset;
        if (effect[0] == 0x21) {
            lastActive[side] = now;
            releaseAt[side] = 0;
        } else if (resetGrace > 0 && (effect[0] == 5 || effect[0] == 0) &&
                   dsb.pending[offset] == 0x21 && now - lastActive[side] < resetGrace) {
            releaseAt[side] = lastActive[side] + resetGrace;
            memcpy(releaseEffect[side], effect, 11);
            suppressedResets[side]++;
            continue;
        } else {
            releaseAt[side] = 0;
        }
        dsb.pending[0] |= bit;
        memcpy(dsb.pending + offset, effect, 11);
    }
    if (src[1] & 4) {
        dsb.pending[1] |= 4;
        memcpy(dsb.pending + 44, src + 44, 3);
    }
    if (src[1] & 0x10) {
        dsb.pending[1] |= 0x10;
        dsb.pending[43] = src[43];
    }
    if (src[1] & 1) {
        dsb.pending[1] |= 1;
        dsb.pending[8] = src[8];
    }
    dsb.outputDirty = dsb.pending[0] || dsb.pending[1];
    pthread_mutex_unlock(&dsb.outputLock);
    if (dsb.pcmMode) {
        DSBPCMNotify();
    }
}

static void *outputWorker(void *unused) {
    @autoreleasepool {
        while (atomic_load(&dsb.workerRunning)) {
            @autoreleasepool {
                uint8_t payload[47], snapshot[47];
                bool changed = false;
                pthread_mutex_lock(&dsb.outputLock);
                if (dsb.outputDirty) {
                    memcpy(snapshot, dsb.pending, 47);
                    dsb.outputDirty = false;
                    changed = DSBControlDelta(snapshot, dsb.lastSent, dsb.haveLast, payload);
                }
                pthread_mutex_unlock(&dsb.outputLock);
                if (changed && !atomic_load(&dsb.stalled)) {
                    uint8_t b[78] = {0x31};
                    b[1] = (dsb.btSequence++ & 15) << 4;
                    b[2] = 0x10;
                    memcpy(b + 3, payload, 47);
                    uint8_t seed = 0xa2;
                    uLong crc = crc32(0, &seed, 1);
                    crc = crc32(crc, b, 74);
                    for (int i = 0; i < 4; i++) {
                        b[74 + i] = crc >> (8 * i);
                    }
                    if (!DSBWriteOutput(b, sizeof(b))) {
                        break;
                    }
                    pthread_mutex_lock(&dsb.outputLock);
                    memcpy(dsb.lastSent, snapshot, 47);
                    dsb.haveLast = true;
                    NSMutableArray *r2 = [NSMutableArray array], *l2 = [NSMutableArray array];
                    for (int i = 10; i < 21; i++) {
                        [r2 addObject:@(payload[i])];
                        [l2 addObject:@(payload[i + 11])];
                    }
                    [dsb.sentHistory addObject:@{
                        @"at" : @([[NSDate date] timeIntervalSince1970]),
                        @"r2" : r2,
                        @"l2" : l2
                    }];
                    if (dsb.sentHistory.count > 40) {
                        [dsb.sentHistory removeObjectAtIndex:0];
                    }
                    pthread_mutex_unlock(&dsb.outputLock);
                }
            }
            usleep(16000);
        }
    }
    atomic_store(&dsb.workerExited, true);
    return NULL;
}

static bool setLevel(int ch, float level) {
    return DSBHapticSet(&handles[ch], level);
}

static bool prepare(GCController *pad) {
    return DSBHapticOpen(&handles[0], pad, GCHapticsLocalityLeftHandle) &&
           DSBHapticOpen(&handles[1], pad, GCHapticsLocalityRightHandle);
}

static bool renewPlayers(void) {
    return DSBHapticRenew(&handles[0]) && DSBHapticRenew(&handles[1]);
}

static bool moved(uint8_t value, uint8_t sent) {
    return value != sent && (!value || abs((int)value - (int)sent) >= 4);
}

static int openController(DSBProbe *probe, IOHIDManagerRef *owner) {
    if (session.dry) {
        return 0;
    }
    IOHIDManagerRef manager = NULL;
    unsigned bt = 0, usb = 0;
    manager = IOHIDManagerCreate(NULL, dsb.pcmMode ? kIOHIDManagerOptionIndependentDevices : 0);
    *owner = manager;
    if (!manager) {
        return 4;
    }
    // Both DualSense models: the standard pad (0ce6) and the Edge (0df2).
    IOHIDManagerSetDeviceMatchingMultiple(
        manager, (__bridge CFArrayRef) @[
            @{@kIOHIDVendorIDKey : @0x54c, @kIOHIDProductIDKey : @0xce6},
            @{@kIOHIDVendorIDKey : @0x54c, @kIOHIDProductIDKey : @0xdf2}
        ]);
    if (IOHIDManagerOpen(manager, 0)) {
        return 4;
    }
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices) {
        for (id obj in (__bridge NSSet *)devices) {
            IOHIDDeviceRef d = (__bridge IOHIDDeviceRef)obj;
            NSString *transport =
                (__bridge NSString *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDTransportKey));
            if ([transport isEqualToString:@"USB"]) {
                usb++;
            } else if ([transport hasPrefix:@"Bluetooth"]) {
                bt++;
                dsb.hid = d;
                NSNumber *product =
                    (__bridge NSNumber *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductIDKey));
                dsb.product = product ? product.unsignedShortValue : 0xce6;
            }
        }
        if (dsb.hid) {
            CFRetain(dsb.hid);
        }
        CFRelease(devices);
    }
    if (probe) {
        *probe = (DSBProbe){.bluetooth = bt,
                            .usb = usb,
                            .eligible = bt == 1 && !usb,
                            .vendor = bt == 1 ? 0x54c : 0,
                            .product = bt == 1 ? dsb.product : 0};
        return 0;
    }
    if (bt != 1 || usb) {
        fprintf(stderr, "Requires exactly one Bluetooth DualSense and no USB DualSense\n");
        return 3;
    }
    if (IOHIDDeviceOpen(dsb.hid, 0)) {
        return 4;
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    GCController *pad = nil;
    // The raw PCM transport owns HID output. Do not also create a native
    // controller/haptic client: its initialization and teardown can write
    // competing mode/LED commands. Core Haptics is an explicit fallback only.
    if (!dsb.pcmMode) {
        GCController.shouldMonitorBackgroundEvents = YES;
        for (int i = 0; i < 80 && !pad; i++) {
            for (GCController *c in GCController.controllers) {
                if ([c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) {
                    pad = c;
                    break;
                }
            }
            if (!pad) {
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
            }
        }
        nativePad = (GCDualSenseGamepad *)pad.extendedGamepad;
        if (!pad || !prepare(pad)) {
            fprintf(stderr, "DualSense Core Haptics initialization failed\n");
            return 5;
        }
    }
    if (dsb.pcmMode) {
        if (!DSBPCMStartInput()) {
            return 8;
        }
    }
    if (!dsb.pcmMode) {
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidDisconnectNotification
                        object:pad
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *n) {
                      fprintf(stderr, "Core Haptics controller disconnected\n");
                      session.quitting = 1;
                    }];
    }

    return 0;
}

// Scope cleanup covers probe and every startup failure before the event loop.
// A stalled driver call is the exception: the process exits without touching
// that IOKit connection again, since closing it could block on the same driver.
static void releaseControllerManager(IOHIDManagerRef *manager) {
    if (*manager && !atomic_load(&dsb.stalled)) {
        IOHIDManagerClose(*manager, 0);
        CFRelease(*manager);
    }
}

static bool dualSenseArgument(int argc, char **argv, int *i) {
    if (!strcmp(argv[*i], "--pcm")) {
        dsb.pcmMode = true;
        return true;
    }
    if (!strcmp(argv[*i], "--trigger-reset-grace-ms") && *i + 1 < argc) {
        char *end = NULL;
        long ms = strtol(argv[++*i], &end, 10);
        if (end == argv[*i] || *end || ms < 0 || ms > 100) {
            return false;
        }
        resetGrace = ms / 1000.;
        return true;
    }
    return false;
}

// Both models: the standard pad (054c:0ce6) and the Edge (054c:0df2).
static int dualSenseProbe(DSBProbe *found) {
    *found = (DSBProbe){0};
    int result = openController(found, &manager);
    if (dsb.hid) {
        CFRelease(dsb.hid);
        dsb.hid = NULL;
    }
    releaseControllerManager(&manager);
    manager = NULL;
    return result;
}

static int dualSenseOpen(void) {
    dsb.sentHistory = [NSMutableArray array];
    int result = openController(NULL, &manager);
    if (result) {
        if (dsb.hid) {
            CFRelease(dsb.hid);
            dsb.hid = NULL;
        }
        releaseControllerManager(&manager);
        manager = NULL;
    }
    return result;
}

// Its triggers resist and vibrate either way; the voice coils and the pad's input
// reports come with the PCM transport.
static unsigned dualSenseAbilities(void) {
    unsigned triggers = DSB_CAN_TRIGGER_FORCE | DSB_CAN_TRIGGER_RUMBLE;
    return triggers | (dsb.pcmMode ? DSB_CAN_HAPTICS | DSB_CAN_AUDIO | DSB_CAN_READ_TRIGGERS : 0);
}

static bool dualSenseRealTime(void) {
    return dsb.pcmMode;
}

static bool dualSenseStart(void) {
    if (dsb.pcmMode) {
        task_category_policy_data_t policy = {TASK_FOREGROUND_APPLICATION};
        kern_return_t result =
            task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&policy,
                            TASK_CATEGORY_POLICY_COUNT);
        fprintf(stderr, "PCM task scheduling role result=%d\n", result);
    }
    atomic_store(&dsb.workerRunning, true);
    if (pthread_create(&worker, NULL, dsb.pcmMode ? DSBPCMWorker : outputWorker, NULL)) {
        fprintf(stderr, "Controller writer thread unavailable\n");
        return false;
    }
    if (dsb.pcmMode) {
        double deadline = CFAbsoluteTimeGetCurrent() + 5;
        while (!DSBPCMWorkerIsReady() && CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .002, true);
        }
        if (!DSBPCMWorkerIsReady() || atomic_load(&dsb.stalled)) {
            fprintf(stderr, "PCM writer did not initialize\n");
            atomic_store(&dsb.workerRunning, false);
            atomic_store(&dsb.stalled, true);
            return false;
        }
    }
    if (session.rumbleHaptics) {
        fprintf(stderr, "Rumble haptics: motor values play on the voice coils "
                        "(%.0f/%.0f Hz) while no haptic audio plays\n",
                DSB_RUMBLE_LOW_HZ, DSB_RUMBLE_HIGH_HZ);
    }
    return true;
}

static bool dualSenseAccepts(const DSBPacket *packet) {
    switch (packet->kind) {
    case DSB_PCM:
        return dsb.pcmMode && (packet->report[0] == 0 || packet->report[0] == 30);
    case DSB_OUTPUT:
        return packet->report[0] == 2;
    case DSB_INPUT:
        return session.dry && packet->report[0] == 0x31;
    default:
        return false;
    }
}

static void dualSenseReceive(const DSBPacket *packet, DSBGameOutput *out) {
    const DSBPacket p = *packet;
    if (p.kind == DSB_PCM) {
        DSBPCMReceive(&p);
        return;
    }
    if (p.kind == DSB_INPUT) {
        DSBNotePadInput(p.report, sizeof(p.report));
        return;
    }
    const uint8_t *b = p.report + 1;
    // What a game asks of the pad, even what is not acted on: each new
    // combination of the valid flags (0, 1, 2) and the motor bytes, once.
    {
        static uint64_t seen[40];
        static unsigned count;
        uint64_t k = (uint64_t)b[0] | (uint64_t)b[1] << 8 | (uint64_t)b[38] << 16 |
                     (uint64_t)b[2] << 24 | (uint64_t)b[3] << 32 | (uint64_t)p.pid << 40;
        bool known = false;
        for (unsigned i = 0; i < count; i++) {
            known |= seen[i] == k;
        }
        if (!known && count < 40) {
            seen[count++] = k;
            fprintf(stderr,
                    "Output source=%u flags=%02x/%02x/%02x motors R=%u L=%u triggers R=%02x L=%02x\n",
                    p.pid, b[0], b[1], b[38], b[2], b[3], b[10], b[21]);
        }
    }
    if (b[1] & 4) {
        if (!ledCommands || memcmp(ledRGB, b + 44, 3) || ledSource != p.pid) {
            fprintf(stderr, "LED source=%u rgb=%u,%u,%u flags=%02x/%02x\n", p.pid, b[44], b[45],
                    b[46], b[0], b[1]);
        }
        ledCommands++;
        ledSource = p.pid;
        memcpy(ledRGB, b + 44, 3);
    }
    if (b[1] & 16) {
        if (!playerCommands || playerLED != b[43]) {
            fprintf(stderr, "Player LED source=%u value=%02x flags=%02x/%02x\n", p.pid, b[43], b[0],
                    b[1]);
        }
        playerCommands++;
        playerLED = b[43];
    }
    queueOutput(p.report);
    if (out) {
        out->triggers = (b[0] >> 2) & 3;
        // LED/trigger-only transactions leave the last rumble intact.
        if ((b[0] & 3) || (b[38] & 4)) {
            out->motors = true;
            out->left = b[3] / 255.f;
            out->right = b[2] / 255.f;
        }
    }
}

static bool dualSenseUpdate(double now, const DSBMotorState *motors, float pcmGain,
                            SessionMetrics *metrics) {
    pthread_mutex_lock(&dsb.outputLock);
    for (int side = 0; side < 2; side++) {
        if (releaseAt[side] && now >= releaseAt[side]) {
            dsb.pending[0] |= 4u << side;
            memcpy(dsb.pending + 10 + side * 11, releaseEffect[side], 11);
            releaseAt[side] = 0;
            dsb.outputDirty = true;
        }
    }
    pthread_mutex_unlock(&dsb.outputLock);
    float l = motors->left, r = motors->right;
    if (!dsb.pcmMode) {
        l = fmaxf(l, DSBCalibratedPCM(motors->envelopeLeft, pcmGain));
        r = fmaxf(r, DSBCalibratedPCM(motors->envelopeRight, pcmGain));
    }
    if (dsb.pcmMode) {
        float a = motors->left, b = motors->right;
        if (session.rumbleHaptics) {
            // Only the game's motor values: the envelope below is real haptic
            // audio, which the writer already plays and must not be played twice.
            // Under 4 of 255 is the motors path's deadband; it would only keep a
            // stream of silence open here.
            uint8_t sl = (uint8_t)lrintf(a * 255), sr = (uint8_t)lrintf(b * 255);
            atomic_store(&dsb.synthLeft, sl < 4 ? 0 : sl);
            atomic_store(&dsb.synthRight, sr < 4 ? 0 : sr);
        }
        // While the link is too slow for PCM, the haptic audio would otherwise be
        // dropped; its envelope drives the motors instead, as it does without PCM.
        if (DSBPCMSuspended() && (motors->envelopeLeft > 0 || motors->envelopeRight > 0)) {
            float el = DSBCalibratedPCM(motors->envelopeLeft, pcmGain),
                  er = DSBCalibratedPCM(motors->envelopeRight, pcmGain);
            if (el > a || er > b) {
                metrics->envelopeUpdates++;
            }
            a = fmaxf(a, el);
            b = fmaxf(b, er);
        }
        l = fmaxf(l, a);
        r = fmaxf(r, b);
        uint8_t left = (uint8_t)lrintf(a * 255), right = (uint8_t)lrintf(b * 255);
        pthread_mutex_lock(&dsb.outputLock);
        // A change under 4 of 255 is not felt but costs a Bluetooth write; the
        // envelope moves on every audio buffer. Reaching zero always goes out.
        if (moved(left, dsb.legacyLeft) || moved(right, dsb.legacyRight)) {
            dsb.legacyLeft = left;
            dsb.legacyRight = right;
            dsb.legacyDirty = true;
        }
        pthread_mutex_unlock(&dsb.outputLock);
        DSBPCMNotify();
    }
    metrics->peakLeft = fmaxf(metrics->peakLeft, l);
    metrics->peakRight = fmaxf(metrics->peakRight, r);
    if (!dsb.pcmMode) {
        if (!renewPlayers()) {
            return false;
        }
        if (!setLevel(0, l) || !setLevel(1, r)) {
            return false;
        }
    }
    metrics->left = l;
    metrics->right = r;
    return true;
}

// The pad's trigger modes for a choice (Controls.h): a fixed effect, the 40 Hz
// vibration of a rumble choice at its amplitude, or off.
static void dualSenseTrigger(int side, uint8_t kind, uint8_t strength) {
    uint8_t effect[11] = {5};
    if (kind == DSB_TRIGGER_RUMBLE) {
        DSBTriggerVibration(2, strength, DSB_TRIGGER_RUMBLE_HZ, effect);
    } else {
        DSBTriggerPreset(kind, strength, effect);
    }
    pthread_mutex_lock(&dsb.outputLock);
    dsb.pending[0] |= 4u << side;
    memcpy(dsb.pending + 10 + side * 11, effect, 11);
    dsb.outputDirty = true;
    pthread_mutex_unlock(&dsb.outputLock);
    if (dsb.pcmMode) {
        DSBPCMNotify();
    }
}

static bool dualSenseFailed(void) {
    // A writer that ended while it should still run is a stall whatever ended it:
    // nothing else writes to the pad for the rest of the session.
    if (atomic_load(&dsb.workerExited) && atomic_load(&dsb.workerRunning) &&
        !atomic_exchange(&dsb.stalled, true)) {
        fprintf(stderr, "Output writer ended while the session runs\n");
    }
    return DSBOutputWriteTimedOut() || atomic_load(&dsb.stalled);
}

static void dualSenseStatus(NSMutableDictionary *obj) {
    NSDictionary *pcmStats = DSBPCMSnapshot();
    pthread_mutex_lock(&dsb.outputLock);
    NSMutableArray *r2 = [NSMutableArray array], *l2 = [NSMutableArray array];
    for (int i = 10; i < 21; i++) {
        [r2 addObject:@(dsb.pending[i])];
        [l2 addObject:@(dsb.pending[i + 11])];
    }
    NSDictionary *triggers = @{
        @"resetGraceMs" : @(resetGrace * 1000),
        @"suppressedR2Resets" : @(suppressedResets[0]),
        @"suppressedL2Resets" : @(suppressedResets[1]),
        @"r2" : r2,
        @"l2" : l2,
        @"updates" : @{@"r2" : @(triggerUpdates[0]), @"l2" : @(triggerUpdates[1])},
        @"sentHistory" : [dsb.sentHistory copy],
        @"physicalR2Mode" : @(dsb.pcmMode ? -1 : nativePad.rightTrigger.mode),
        @"physicalR2Status" : @(dsb.pcmMode ? -1 : nativePad.rightTrigger.status)
    };
    pthread_mutex_unlock(&dsb.outputLock);
    obj[@"led"] = @{
        @"commands" : @(ledCommands),
        @"sourcePid" : @(ledSource),
        @"rgb" : @[ @(ledRGB[0]), @(ledRGB[1]), @(ledRGB[2]) ],
        @"playerCommands" : @(playerCommands),
        @"player" : @(playerLED)
    };
    obj[@"pcm"] = pcmStats;
    obj[@"triggers"] = triggers;
    obj[@"hidReports"] = @(atomic_load(&dsb.reports));
    obj[@"hidErrors"] = @(atomic_load(&dsb.errors));
    obj[@"hidStalled"] = @(atomic_load(&dsb.stalled));
}

static void dualSenseCounters(unsigned *writes, unsigned *errors) {
    *writes = atomic_load(&dsb.reports);
    *errors = atomic_load(&dsb.errors);
}

// A stalled driver call is the exception to every cleanup here: the process exits
// without touching that IOKit connection again, since closing it could block on
// the same driver.
static int dualSenseStop(NSString **state) {
    if (!dsb.pcmMode) {
        setLevel(0, 0);
        setLevel(1, 0);
    }
    // Release triggers only if this session touched them. Leave unrelated Steam output alone.
    resetGrace = 0; // Cleanup must never defer release.
    if (!atomic_load(&dsb.stalled) && !atomic_load(&dsb.deviceRemoved) &&
        (dsb.pending[0] & 12)) {
        uint8_t reset[64];
        DSBTriggerRelease(dsb.pending[0], reset);
        queueOutput(reset);
        double deadline = CFAbsoluteTimeGetCurrent() + .25;
        while (!atomic_load(&dsb.stalled) && CFAbsoluteTimeGetCurrent() < deadline) {
            uint8_t unsent[47];
            pthread_mutex_lock(&dsb.outputLock);
            bool pending = DSBControlDelta(dsb.pending, dsb.lastSent, dsb.haveLast, unsent);
            pthread_mutex_unlock(&dsb.outputLock);
            if (!pending || DSBOutputWriteTimedOut()) {
                break;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .002, true);
        }
    }
    atomic_store(&dsb.workerRunning, false);
    while (!atomic_load(&dsb.workerExited) && !atomic_load(&dsb.stalled)) {
        DSBOutputWriteTimedOut();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .002, true);
    }
    if (atomic_load(&dsb.workerExited)) {
        pthread_join(worker, NULL);
    }
    DSBHapticClose(&handles[0]);
    DSBHapticClose(&handles[1]);
    if (dsb.hid && !atomic_load(&dsb.stalled)) {
        if (dsb.pcmMode) {
            IOHIDDeviceUnscheduleFromRunLoop(dsb.hid, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        }
        IOHIDDeviceClose(dsb.hid, 0);
        CFRelease(dsb.hid);
    }
    releaseControllerManager(&manager);
    manager = NULL;
    *state = atomic_load(&dsb.deviceRemoved) ? @"disconnected"
             : atomic_load(&dsb.stalled)     ? @"output-stopped"
                                             : @"stopped";
    return atomic_load(&dsb.deviceRemoved) ? 10 : atomic_load(&dsb.stalled) ? 11 : 0;
}

const DSBBackend DSBDualSenseBackend = {
    .name = "dualsense",
    .argument = dualSenseArgument,
    .probe = dualSenseProbe,
    .open = dualSenseOpen,
    .abilities = dualSenseAbilities,
    .realTime = dualSenseRealTime,
    .start = dualSenseStart,
    .accepts = dualSenseAccepts,
    .receive = dualSenseReceive,
    .update = dualSenseUpdate,
    .trigger = dualSenseTrigger,
    .failed = dualSenseFailed,
    .status = dualSenseStatus,
    .counters = dualSenseCounters,
    .stop = dualSenseStop,
};
