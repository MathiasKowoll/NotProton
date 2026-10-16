// The controller broker's device-neutral core: one session's authenticated
// packets and the sources they come from, the motor values folded from them,
// the per-title trigger and rumble options, and the status file. Everything a
// pad does is its backend's (Session.h; the DualSense's is DualSense.m).
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include "Session.h"
#include "Calibration.h"
#include "TriggerSpec.h"

// Local per-session service. Only receives packets bearing the launch token.
DSBSession session;
// The backends this broker can run; --backend names one, the DualSense is the
// default, and a probe asks them in order.
static const DSBBackend *const backends[] = {&DSBDualSenseBackend, &DSBGameControllerBackend};
static const DSBBackend *backend = &DSBDualSenseBackend;
static bool backendChosen;
static unsigned abilities;
// --triggers: per-trigger effects for titles that write none (kind[0] R2,
// kind[1] L2). A side the game writes itself becomes the game's for good.
static const char *triggerSpec;
static DSBTriggerSpec triggerChoice;
// A side with an effect for while the other trigger is pressed: whether that
// trigger is held now, and how often the side changed effect.
static bool waitHeld[2];
static uint64_t triggerWaitChanges;
static bool gameOwnsTrigger[2];
static uint8_t triggerRumbleStep[2];
static double triggerRumbleAt[2];
static uint64_t triggerRumbleUpdates;

static void stopSignal(int s) {
    session.quitting = 1;
}
// Read readiness wakes the run loop immediately; the main loop drains and
// authenticates datagrams before rearming this source.
static void socketReady(CFFileDescriptorRef descriptor, CFOptionFlags flags, void *context) {}
typedef struct {
    uint32_t pid, sequence;
    double lastLevelAt, lastRumbleAt, lastPacketAt;
    float levelLeft, levelRight, rumbleLeft, rumbleRight;
    bool initialized;
} Source;
static Source sources[64];

static bool validPayload(const DSBPacket *packet) {
    switch (packet->kind) {
    case DSB_LEVEL:
    case DSB_MOTORS:
        return true;
    case DSB_OUTPUT:
    case DSB_PCM:
    case DSB_INPUT:
        return backend->accepts(packet);
    default:
        return false;
    }
}

static void receivePackets(int fd, const uint8_t token[16], double now, SessionMetrics *metrics) {
    for (int j = 0; j < 1024; j++) {
        DSBPacket p;
        uint8_t datagram[sizeof(p) + 1];
        ssize_t n = recv(fd, datagram, sizeof(datagram), 0);
        if (n < 0) {
            break;
        }
        memcpy(&p, datagram, MIN((size_t)n, sizeof(p)));
        if (n != sizeof(p) || p.magic != DSB_MAGIC || memcmp(p.token, token, 16) ||
            !isfinite(p.timestamp) || fabs(now - p.timestamp) > .25 || !isfinite(p.left) ||
            !isfinite(p.right) || !p.pid || !validPayload(&p)) {
            metrics->dropped++;
            continue;
        }
        Source *s = NULL;
        for (int k = 0; k < 64; k++) {
            if (sources[k].pid == p.pid) {
                s = sources + k;
                break;
            }
        }
        if (!s) {
            for (int k = 0; k < 64; k++) {
                if (!sources[k].pid || now - sources[k].lastPacketAt > 2) {
                    s = sources + k;
                    memset(s, 0, sizeof(*s));
                    s->pid = p.pid;
                    break;
                }
            }
        }
        if (!s || (s->initialized && (int32_t)(p.sequence - s->sequence) <= 0)) {
            metrics->dropped++;
            continue;
        }
        s->initialized = true;
        s->lastPacketAt = now;
        s->sequence = p.sequence;
        if (p.kind == DSB_LEVEL) {
            s->lastLevelAt = now;
            s->levelLeft = fmaxf(0, fminf(1, p.left));
            s->levelRight = fmaxf(0, fminf(1, p.right));
            if (!(abilities & DSB_CAN_AUDIO)) {
                metrics->levels++;
            }
        } else if (p.kind == DSB_PCM) {
            backend->receive(&p, NULL);
            s->lastLevelAt = now;
            metrics->levels++;
        } else if (p.kind == DSB_OUTPUT) {
            DSBGameOutput out = {0};
            backend->receive(&p, &out);
            metrics->outputs++;
            for (int side = 0; side < 2; side++) {
                if ((out.triggers & (1u << side)) && !session.quitting) {
                    gameOwnsTrigger[side] = true;
                }
            }
            if (out.motors) {
                s->lastRumbleAt = now;
                s->rumbleLeft = out.left;
                s->rumbleRight = out.right;
            }
        } else if (p.kind == DSB_MOTORS) {
            float left = fmaxf(0, fminf(1, p.left)), right = fmaxf(0, fminf(1, p.right));
            // Each new pair a source asks for, once, as the output log does.
            {
                static uint64_t seen[40];
                static unsigned count;
                uint64_t k = (uint64_t)lrintf(left * 255) | (uint64_t)lrintf(right * 255) << 8 |
                             (uint64_t)p.pid << 16;
                bool known = false;
                for (unsigned i = 0; i < count; i++) {
                    known |= seen[i] == k;
                }
                if (!known && count < 40) {
                    seen[count++] = k;
                    fprintf(stderr, "Motors source=%u L=%ld R=%ld\n", p.pid, lrintf(left * 255),
                            lrintf(right * 255));
                }
            }
            s->lastRumbleAt = now;
            s->rumbleLeft = left;
            s->rumbleRight = right;
            metrics->outputs++;
        } else if (p.kind == DSB_INPUT) {
            backend->receive(&p, NULL);
        } else {
            metrics->dropped++;
            continue;
        }
        metrics->accepted++;
    }
}

// A wired pad holds the last motor values until it is told otherwise, and games
// write 0x02 when the value changes. Expiring a value half a second after it was
// written cut every longer rumble short. It is held while its source is alive:
// any packet from it in the last two seconds, the same window that frees a slot.
static bool rumbleHeld(const Source *s, double now) {
    return s->lastRumbleAt &&
           (now - s->lastRumbleAt < .5 || now - s->lastPacketAt < 2);
}

// A trigger set to rumble vibrates with the game's motor: R2 with the light
// right motor, L2 with the heavy left one, in eight steps. A step changes at
// most every 60 ms, so a game's rumble ramp costs a handful of control writes.
static void updateTriggerRumble(double now, uint8_t right, uint8_t left) {
    const uint8_t motor[2] = {right, left};
    for (int side = 0; side < 2; side++) {
        uint8_t kind, strength;
        DSBTriggerActive(&triggerChoice, side, waitHeld[side], &kind, &strength);
        if (kind != DSB_TRIGGER_RUMBLE || gameOwnsTrigger[side]) {
            continue;
        }
        uint8_t step = DSBTriggerRumbleStep(motor[side]);
        if (step == triggerRumbleStep[side] || now - triggerRumbleAt[side] < .06) {
            continue;
        }
        backend->trigger(side, DSB_TRIGGER_RUMBLE, DSBTriggerRumbleAmplitude(step, strength));
        triggerRumbleStep[side] = step;
        triggerRumbleAt[side] = now;
        triggerRumbleUpdates++;
    }
}

// A side with an effect for while the other trigger is pressed
// ("R2:resist@2,R2:weapon@8+L2"): it switches between its two effects as that
// trigger is pressed and released, read from the pad's own input. No effect is
// a released trigger. A rumble effect starts from still and the rumble update
// moves it.
static void updateTriggerWaits(void) {
    for (int side = 0; side < 2; side++) {
        if (!triggerChoice.when[side] || gameOwnsTrigger[side]) {
            continue;
        }
        uint8_t other = atomic_load(&session.padTrigger[triggerChoice.when[side] - 1]);
        bool held = DSBTriggerPressed(waitHeld[side], other);
        if (held == waitHeld[side]) {
            continue;
        }
        waitHeld[side] = held;
        triggerWaitChanges++;
        uint8_t kind, strength;
        DSBTriggerActive(&triggerChoice, side, held, &kind, &strength);
        backend->trigger(side, kind == DSB_TRIGGER_RUMBLE ? DSB_TRIGGER_NONE : kind, strength);
        triggerRumbleStep[side] = 0;
        triggerRumbleAt[side] = 0;
    }
}

static bool updateEffects(double now, float pcmGain, SessionMetrics *metrics) {
    DSBMotorState motors = {0};
    for (int i = 0; i < 64; i++) {
        Source *s = sources + i;
        if (rumbleHeld(s, now)) {
            motors.left = fmaxf(motors.left, s->rumbleLeft);
            motors.right = fmaxf(motors.right, s->rumbleRight);
        }
        if (now - s->lastLevelAt < .15) {
            motors.envelopeLeft = fmaxf(motors.envelopeLeft, s->levelLeft);
            motors.envelopeRight = fmaxf(motors.envelopeRight, s->levelRight);
        }
    }
    updateTriggerWaits();
    updateTriggerRumble(now, (uint8_t)lrintf(motors.right * 255),
                        (uint8_t)lrintf(motors.left * 255));
    return backend->update(now, &motors, pcmGain, metrics);
}

static void writeStatus(NSString *status, unsigned port, float pcmGain,
                        const SessionMetrics *metrics) {
    NSMutableDictionary *obj = [@{
        @"state" : @"running",
        @"backend" : @(backend->name),
        @"pid" : @(getpid()),
        @"port" : @(port),
        @"accepted" : @(metrics->accepted),
        @"dropped" : @(metrics->dropped),
        @"pcmBlocks" : @(metrics->levels),
        @"envelopeUpdates" : @(metrics->envelopeUpdates),
        @"pcmGain" : @(pcmGain),
        @"rumbleHaptics" : @(session.rumbleHaptics),
        @"triggerSpec" : triggerSpec ? @(triggerSpec) : [NSNull null],
        @"triggerRumbleUpdates" : @(triggerRumbleUpdates),
        @"gameOwnsTriggers" : @[ @(gameOwnsTrigger[0]), @(gameOwnsTrigger[1]) ],
        @"triggerWaits" : @[
            triggerChoice.when[0] ? @"L2" : [NSNull null],
            triggerChoice.when[1] ? @"R2" : [NSNull null]
        ],
        @"triggerWaitHeld" : @[ @(waitHeld[0]), @(waitHeld[1]) ],
        @"triggerWaitChanges" : @(triggerWaitChanges),
        @"padTriggers" : @{
            @"r2" : @(atomic_load(&session.padTrigger[0])),
            @"l2" : @(atomic_load(&session.padTrigger[1])),
            @"reports" : @(atomic_load(&session.padTriggerReports))
        },
        @"outputCommands" : @(metrics->outputs),
        @"left" : @(metrics->left),
        @"right" : @(metrics->right),
        @"peakLeft" : @(metrics->peakLeft),
        @"peakRight" : @(metrics->peakRight),
        @"updated" : @([[NSDate date] timeIntervalSince1970])
    } mutableCopy];
    backend->status(obj);
    [[NSJSONSerialization dataWithJSONObject:obj options:NSJSONWritingPrettyPrinted
                                       error:NULL] writeToFile:status atomically:YES];
}

typedef struct {
    NSString *status, *secret;
    pid_t parent;
    double seconds;
    bool probe;
    unsigned requestedPort;
} BrokerOptions;

static bool parseArguments(int argc, char **argv, BrokerOptions *options) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) {
            options->probe = true;
        } else if (!strcmp(argv[i], "--rumble-haptics")) {
            session.rumbleHaptics = true;
        } else if (!strcmp(argv[i], "--triggers") && i + 1 < argc) {
            if (!DSBParseTriggers(argv[++i], &triggerChoice)) {
                return false;
            }
            triggerSpec = argv[i];
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            const DSBBackend *named = NULL;
            for (size_t b = 0; b < sizeof(backends) / sizeof(backends[0]); b++) {
                if (!strcmp(backends[b]->name, argv[i + 1])) {
                    named = backends[b];
                }
            }
            if (!named) {
                return false;
            }
            backend = named;
            backendChosen = true;
            i++;
        } else if (!strcmp(argv[i], "--dry-run")) {
            session.dry = true;
        } else if (!strcmp(argv[i], "--token") && i + 1 < argc) {
            options->secret = @(argv[++i]);
        } else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
            options->status = @(argv[++i]);
        } else if (!strcmp(argv[i], "--parent") && i + 1 < argc) {
            char *end = NULL;
            long parent = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end || parent <= 0 || parent > INT_MAX) {
                return false;
            }
            options->parent = (pid_t)parent;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            char *end = NULL;
            options->seconds = strtod(argv[++i], &end);
            if (end == argv[i] || *end || !isfinite(options->seconds) || options->seconds <= 0) {
                return false;
            }
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            char *end = NULL;
            long p = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end || p < 1024 || p > 65535) {
                return false;
            }
            options->requestedPort = (unsigned)p;
        } else if (!backend->argument(argc, argv, &i)) {
            return false;
        }
    }
    return true;
}

// The options the pad cannot honour are declined here, each with a log line,
// and the fixed trigger effects go out before the session reports ready.
static void applyTitleOptions(void) {
    if (session.rumbleHaptics && !(abilities & DSB_CAN_HAPTICS)) {
        session.rumbleHaptics = false;
        fprintf(stderr, "Rumble haptics need the PCM transport; motors only\n");
    }
    if (!triggerSpec) {
        return;
    }
    static const char *const kindNames[] = {"untouched", "light",   "medium",
                                            "strong",    "weapon",  "slope",
                                            "vibrate",   "rumble",  "resist"};
    static const char *const sideNames[] = {"R2", "L2"};
    for (int side = 0; side < 2; side++) {
        if (triggerChoice.when[side] && !(abilities & DSB_CAN_READ_TRIGGERS)) {
            // Only a pad whose input the backend reads can say when to switch.
            fprintf(stderr,
                    "Trigger waits need the PCM transport; %s keeps only its first effect\n",
                    sideNames[side]);
            triggerChoice.when[side] = triggerChoice.held[side] = 0;
            triggerChoice.heldStrength[side] = 0;
        }
    }
    // A choice the pad's triggers cannot play is left to the game.
    for (int side = 0; side < 2; side++) {
        uint8_t *kinds[2] = {&triggerChoice.kind[side], &triggerChoice.held[side]};
        for (int which = 0; which < 2; which++) {
            uint8_t kind = *kinds[which];
            bool resists = DSBTriggerResists(kind);
            if (kind == DSB_TRIGGER_NONE ||
                (abilities & (resists ? DSB_CAN_TRIGGER_FORCE : DSB_CAN_TRIGGER_RUMBLE))) {
                continue;
            }
            fprintf(stderr, "Triggers: %s cannot %s on %s; %s left to the game\n", backend->name,
                    resists ? "resist" : "vibrate", sideNames[side],
                    which ? "its effect while the other is pressed" : "its effect");
            *kinds[which] = DSB_TRIGGER_NONE;
        }
        if (triggerChoice.when[side] && triggerChoice.held[side] == DSB_TRIGGER_NONE) {
            triggerChoice.when[side] = 0;
            triggerChoice.heldStrength[side] = 0;
        }
    }
    // Fixed effects go out now; a rumble side follows the motors. The game's own
    // trigger writes replace either as they arrive, and the session's cleanup
    // releases them like any trigger it set.
    for (int side = 0; side < 2; side++) {
        uint8_t kind = triggerChoice.kind[side];
        if (kind != DSB_TRIGGER_NONE && kind != DSB_TRIGGER_RUMBLE) {
            backend->trigger(side, kind, triggerChoice.strength[side]);
        }
    }
    for (int side = 0; side < 2; side++) {
        fprintf(stderr, "Triggers %s: %s %s (strength %u)", triggerSpec, sideNames[side],
                kindNames[triggerChoice.kind[side]], triggerChoice.strength[side]);
        if (triggerChoice.when[side]) {
            fprintf(stderr, ", %s (strength %u) while %s is pressed",
                    kindNames[triggerChoice.held[side]], triggerChoice.heldStrength[side],
                    sideNames[1 - side]);
        }
        fputc('\n', stderr);
    }
}

// One JSON line for the launcher: the backend whose pad a session can take, or
// the first one asked when none can, with what it found.
static int probe(void) {
    const DSBBackend *answer = NULL;
    DSBProbe found = {0};
    size_t count = backendChosen ? 1 : sizeof(backends) / sizeof(backends[0]);
    for (size_t b = 0; b < count; b++) {
        const DSBBackend *candidate = backendChosen ? backend : backends[b];
        if (!backendChosen && candidate->onlyWhenNamed) {
            continue;
        }
        DSBProbe p = {0};
        int result = candidate->probe(&p);
        if (result) {
            return result;
        }
        if (!answer || (p.eligible && !found.eligible)) {
            answer = candidate;
            found = p;
        }
        if (found.eligible) {
            break;
        }
    }
    printf("{\"backend\":\"%s\",\"bluetooth\":%u,\"usb\":%u,\"eligible\":%s,"
           "\"vendor\":\"%04x\",\"product\":\"%04x\"}\n",
           answer->name, found.bluetooth, found.usb, found.eligible ? "true" : "false",
           found.vendor, found.product);
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        id activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"DualSense real-time haptic transport"];
        setbuf(stdout, NULL);
        signal(SIGTERM, stopSignal);
        signal(SIGINT, stopSignal);
        BrokerOptions options = {0};
        uint8_t token[16] = {0};
        if (!parseArguments(argc, argv, &options) ||
            (!options.probe && !DSBParseToken(options.secret.UTF8String, token))) {
            return 64;
        }
        NSString *status = options.status;
        if (options.probe) {
            return probe();
        }
        int controllerResult = backend->open();
        if (controllerResult) {
            return controllerResult;
        }
        abilities = backend->abilities();
        bool realTime = backend->realTime();
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return 6;
        }
        struct sockaddr_in addr = {.sin_len = sizeof(addr),
                                   .sin_family = AF_INET,
                                   .sin_port = htons(options.requestedPort),
                                   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (bind(fd, (void *)&addr, sizeof(addr))) {
            return 6;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(fd, (void *)&addr, &len) || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            return 6;
        }
        unsigned port = ntohs(addr.sin_port);
        CFFileDescriptorRef socketEvents = NULL;
        CFRunLoopSourceRef socketSource = NULL;
        if (realTime) {
            socketEvents = CFFileDescriptorCreate(NULL, fd, false, socketReady, NULL);
            if (!socketEvents) {
                fprintf(stderr, "PCM socket event source unavailable\n");
                return 9;
            }
            socketSource = CFFileDescriptorCreateRunLoopSource(NULL, socketEvents, 0);
            if (!socketSource) {
                fprintf(stderr, "PCM socket run loop source unavailable\n");
                return 9;
            }
            CFRunLoopAddSource(CFRunLoopGetMain(), socketSource, kCFRunLoopDefaultMode);
        }
        applyTitleOptions();
        if (!backend->start()) {
            return 9;
        }
        printf("{\"ready\":true,\"port\":%u,\"pid\":%d}\n", port, getpid());
        NSString *calibrationPath = [NSHomeDirectory()
            stringByAppendingPathComponent:
                @"Library/Application Support/notproton/controllers/dualsense/calibration.json"];
        float pcmGain = 1;
        double nextCalibration = 0;
        double start = CFAbsoluteTimeGetCurrent(), nextStatus = 0;
        SessionMetrics metrics = {0};
        while (!session.quitting &&
               (!options.seconds || CFAbsoluteTimeGetCurrent() - start < options.seconds)) {
            @autoreleasepool {
                double now = CFAbsoluteTimeGetCurrent();
                if (options.parent && kill(options.parent, 0) && errno == ESRCH) {
                    break;
                }
                if (backend->failed()) {
                    break;
                }
                if (now >= nextCalibration) {
                    pcmGain = DSBPCMGain(calibrationPath);
                    nextCalibration = now + 1;
                }
                receivePackets(fd, token, now, &metrics);
                if (!updateEffects(now, pcmGain, &metrics)) {
                    break;
                }
                if (status && now >= nextStatus) {
                    writeStatus(status, port, pcmGain, &metrics);
                    nextStatus = now + 1;
                }
                if (realTime) {
                    CFFileDescriptorEnableCallBacks(socketEvents, kCFFileDescriptorReadCallBack);
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, .002, true);
                } else {
                    [[NSRunLoop currentRunLoop]
                        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
                }
            }
        }
        NSString *finalState = @"stopped";
        int exitStatus = backend->stop(&finalState);
        [[NSProcessInfo processInfo] endActivity:activity];
        if (status) {
            writeStatus(status, port, pcmGain, &metrics);
            NSData *data = [NSData dataWithContentsOfFile:status];
            NSMutableDictionary *final =
                data ? [[NSJSONSerialization JSONObjectWithData:data
                                                        options:NSJSONReadingMutableContainers
                                                          error:NULL] mutableCopy]
                     : [NSMutableDictionary dictionary];
            if (!final) {
                final = [NSMutableDictionary dictionary];
            }
            final[@"state"] = finalState;
            final[@"updated"] = @([[NSDate date] timeIntervalSince1970]);
            [[NSJSONSerialization dataWithJSONObject:final
                                             options:NSJSONWritingPrettyPrinted
                                               error:NULL] writeToFile:status atomically:YES];
        }
        if (socketEvents) {
            CFFileDescriptorInvalidate(socketEvents);
            CFRunLoopRemoveSource(CFRunLoopGetMain(), socketSource, kCFRunLoopDefaultMode);
            CFRelease(socketSource);
            CFRelease(socketEvents);
        }
        close(fd);
        unsigned writes = 0, errors = 0;
        backend->counters(&writes, &errors);
        fprintf(stderr, "Stopped: packets=%llu PCM=%llu output=%llu HID=%u errors=%u\n",
                metrics.accepted, metrics.levels, metrics.outputs, writes, errors);
        return exitStatus;
    }
}
