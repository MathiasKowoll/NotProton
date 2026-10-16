// The GameController backend: a pad macOS knows through GameController.framework
// that is not a Sony pad (an Xbox Wireless Controller first). Core Haptics plays
// the motor values on the pad's handles and, where it has trigger haptics, a
// vibrate or rumble choice on its triggers; the pad's own trigger values feed
// the waits. Nothing here speaks the pad's HID protocol.
//
// Nothing here has been measured on a pad. A probe asks this backend only when
// it is named (NOTPROTON_PAD_BACKEND=gamecontroller), so a session without that
// option runs exactly as before. controllers/multi-controller.md, step 4.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDUsageTables.h>
#include <math.h>
#include "Session.h"
#include "TriggerSpec.h"
#include "HapticChannel.h"

static GCController *pad;
static DSBHapticChannel handles[2] = {{.level = -1}, {.level = -1}};
// The pad's triggers (0 right, 1 left), when it has trigger haptics.
static DSBHapticChannel triggers[2] = {{.level = -1}, {.level = -1}};
static bool triggerHaptics;
// What each trigger was asked to play, and whether that trigger is pressed: a
// trigger vibrates only while it is pressed, as a DualSense's vibration mode does.
static uint8_t triggerKind[2], triggerStrength[2];
static bool triggerPressed[2];
static atomic_bool disconnected;
static unsigned writes;

static bool sony(GCController *c) {
    return [c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class] ||
           [c.extendedGamepad isKindOfClass:GCDualShockGamepad.class];
}

// The pads GameController lists that are not Sony's and have haptics. The
// framework fills its list on the run loop, so this waits up to 1.5 s for it.
static NSArray<GCController *> *candidates(void) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    GCController.shouldMonitorBackgroundEvents = YES;
    NSMutableArray<GCController *> *found = [NSMutableArray array];
    for (int i = 0; i < 30; i++) {
        [found removeAllObjects];
        for (GCController *c in GCController.controllers) {
            if (c.extendedGamepad && c.haptics && !sony(c)) {
                [found addObject:c];
            }
        }
        if (found.count) {
            break;
        }
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
    }
    return found;
}

// Its HID identity, for the pieces inside Wine: the one game pad or joystick
// that is not Sony's, with the transport it is on.
static void identify(DSBProbe *found) {
    IOHIDManagerRef manager = IOHIDManagerCreate(NULL, 0);
    if (!manager) {
        return;
    }
    IOHIDManagerSetDeviceMatchingMultiple(
        manager, (__bridge CFArrayRef) @[
            @{
                @kIOHIDDeviceUsagePageKey : @(kHIDPage_GenericDesktop),
                @kIOHIDDeviceUsageKey : @(kHIDUsage_GD_GamePad)
            },
            @{
                @kIOHIDDeviceUsagePageKey : @(kHIDPage_GenericDesktop),
                @kIOHIDDeviceUsageKey : @(kHIDUsage_GD_Joystick)
            }
        ]);
    if (!IOHIDManagerOpen(manager, 0)) {
        CFSetRef devices = IOHIDManagerCopyDevices(manager);
        for (id obj in (__bridge NSSet *)devices) {
            IOHIDDeviceRef d = (__bridge IOHIDDeviceRef)obj;
            NSNumber *vendor = (__bridge NSNumber *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDVendorIDKey));
            NSNumber *product =
                (__bridge NSNumber *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductIDKey));
            NSString *transport =
                (__bridge NSString *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDTransportKey));
            if (!vendor || vendor.unsignedShortValue == 0x54c) {
                continue;
            }
            if ([transport hasPrefix:@"Bluetooth"]) {
                found->bluetooth++;
            } else {
                found->usb++;
            }
            found->vendor = vendor.unsignedShortValue;
            found->product = product.unsignedShortValue;
        }
        if (devices) {
            CFRelease(devices);
        }
        IOHIDManagerClose(manager, 0);
    }
    CFRelease(manager);
}

static bool gameControllerArgument(int argc, char **argv, int *i) {
    return false;
}

// Exactly one such pad, on any transport: Core Haptics drives it the same way.
static int gameControllerProbe(DSBProbe *found) {
    *found = (DSBProbe){0};
    if (session.dry) {
        return 0;
    }
    NSUInteger pads = candidates().count;
    identify(found);
    found->eligible = pads == 1 && found->bluetooth + found->usb == 1;
    return 0;
}

static int gameControllerOpen(void) {
    if (session.dry) {
        return 0;
    }
    NSArray<GCController *> *found = candidates();
    if (found.count != 1) {
        fprintf(stderr, "Requires exactly one game controller with haptics that is not Sony's\n");
        return 3;
    }
    pad = found.firstObject;
    fprintf(stderr, "GameController: %s (%s), haptics on %s\n", pad.vendorName.UTF8String ?: "?",
            pad.productCategory.UTF8String,
            pad.haptics.supportedLocalities.allObjects.description.UTF8String);
    if (!DSBHapticOpen(&handles[0], pad, GCHapticsLocalityLeftHandle) ||
        !DSBHapticOpen(&handles[1], pad, GCHapticsLocalityRightHandle)) {
        return 5;
    }
    NSSet *localities = pad.haptics.supportedLocalities;
    triggerHaptics = [localities containsObject:GCHapticsLocalityLeftTrigger] &&
                     [localities containsObject:GCHapticsLocalityRightTrigger] &&
                     DSBHapticOpen(&triggers[0], pad, GCHapticsLocalityRightTrigger) &&
                     DSBHapticOpen(&triggers[1], pad, GCHapticsLocalityLeftTrigger);
    [[NSNotificationCenter defaultCenter]
        addObserverForName:GCControllerDidDisconnectNotification
                    object:pad
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) {
                  fprintf(stderr, "GameController: the pad disconnected\n");
                  atomic_store(&disconnected, true);
                  session.quitting = 1;
                }];
    return 0;
}

// A dry run stands for a pad with trigger haptics.
static unsigned gameControllerAbilities(void) {
    return DSB_CAN_READ_TRIGGERS | (triggerHaptics || session.dry ? DSB_CAN_TRIGGER_RUMBLE : 0);
}

static bool gameControllerRealTime(void) {
    return false;
}

static bool gameControllerStart(void) {
    return true;
}

// Its pad has no output report a game writes, and no haptic audio. In a dry run
// the DualSense's input fixture stands for the pad's trigger values.
static bool gameControllerAccepts(const DSBPacket *packet) {
    return packet->kind == DSB_INPUT && session.dry && packet->report[0] == 0x31;
}

static void gameControllerReceive(const DSBPacket *packet, DSBGameOutput *out) {
    DSBNoteTriggerPositions(packet->report[7], packet->report[6]);
}

static bool play(DSBHapticChannel *channel, float level) {
    float before = channel->level;
    if (!DSBHapticSet(channel, level)) {
        return false;
    }
    writes += channel->level != before;
    return true;
}

static bool gameControllerUpdate(double now, const DSBMotorState *motors, float gain,
                                 SessionMetrics *metrics) {
    if (pad) {
        GCExtendedGamepad *g = pad.extendedGamepad;
        DSBNoteTriggerPositions((uint8_t)lrintf(g.rightTrigger.value * 255),
                                (uint8_t)lrintf(g.leftTrigger.value * 255));
    }
    if (!DSBHapticRenew(&handles[0]) || !DSBHapticRenew(&handles[1])) {
        return false;
    }
    if (!play(&handles[0], motors->left) || !play(&handles[1], motors->right)) {
        return false;
    }
    if (triggerHaptics || session.dry) {
        for (int side = 0; side < 2; side++) {
            triggerPressed[side] =
                DSBTriggerPressed(triggerPressed[side], atomic_load(&session.padTrigger[side]));
            float level = 0;
            if (triggerKind[side] == DSB_TRIGGER_RUMBLE) {
                level = triggerStrength[side] / 8.f;
            } else if (triggerKind[side] == DSB_TRIGGER_VIBRATE) {
                level = (triggerStrength[side] ? triggerStrength[side] : 3) / 8.f;
            }
            if (!triggerPressed[side]) {
                level = 0;
            }
            if (!DSBHapticRenew(&triggers[side]) || !play(&triggers[side], level)) {
                return false;
            }
        }
    }
    metrics->left = motors->left;
    metrics->right = motors->right;
    metrics->peakLeft = fmaxf(metrics->peakLeft, motors->left);
    metrics->peakRight = fmaxf(metrics->peakRight, motors->right);
    return true;
}

// The core declines what the triggers cannot play, so only vibrate, rumble and
// a release arrive here; they are played on the next turn.
static void gameControllerTrigger(int side, uint8_t kind, uint8_t strength) {
    triggerKind[side] = kind;
    triggerStrength[side] = strength;
}

static bool gameControllerFailed(void) {
    return atomic_load(&disconnected);
}

static void gameControllerStatus(NSMutableDictionary *obj) {
    obj[@"gamecontroller"] = @{
        @"vendorName" : pad.vendorName ?: [NSNull null],
        @"productCategory" : pad.productCategory ?: [NSNull null],
        @"triggerHaptics" : @(triggerHaptics),
        @"handles" : @[ @(handles[0].level), @(handles[1].level) ],
        @"triggers" : @[ @(triggers[0].level), @(triggers[1].level) ],
        @"writes" : @(writes)
    };
}

static void gameControllerCounters(unsigned *count, unsigned *errors) {
    *count = writes;
    *errors = 0;
}

static int gameControllerStop(NSString **state) {
    for (int i = 0; i < 2; i++) {
        DSBHapticSet(&handles[i], 0);
        DSBHapticClose(&handles[i]);
        if (triggerHaptics) {
            DSBHapticSet(&triggers[i], 0);
            DSBHapticClose(&triggers[i]);
        }
    }
    *state = atomic_load(&disconnected) ? @"disconnected" : @"stopped";
    return atomic_load(&disconnected) ? 10 : 0;
}

const DSBBackend DSBGameControllerBackend = {
    .name = "gamecontroller",
    .onlyWhenNamed = true,
    .argument = gameControllerArgument,
    .probe = gameControllerProbe,
    .open = gameControllerOpen,
    .abilities = gameControllerAbilities,
    .realTime = gameControllerRealTime,
    .start = gameControllerStart,
    .accepts = gameControllerAccepts,
    .receive = gameControllerReceive,
    .update = gameControllerUpdate,
    .trigger = gameControllerTrigger,
    .failed = gameControllerFailed,
    .status = gameControllerStatus,
    .counters = gameControllerCounters,
    .stop = gameControllerStop,
};
