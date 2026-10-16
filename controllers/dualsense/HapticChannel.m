#import "HapticChannel.h"
#include <math.h>
#include "Session.h"

bool DSBHapticOpen(DSBHapticChannel *channel, GCController *pad, GCHapticsLocality locality) {
    channel->level = -1;
    channel->engine = [pad.haptics createEngineWithLocality:locality];
    if (!channel->engine) {
        fprintf(stderr, "No haptic engine locality=%s supported=%s\n", [locality UTF8String],
                pad.haptics.supportedLocalities.description.UTF8String);
        return false;
    }
    channel->engine.autoShutdownEnabled = NO;
    channel->engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
      atomic_store(&channel->running, false);
      fprintf(stderr, "Core Haptics stopped: %ld\n", (long)reason);
      session.quitting = 1;
    };
    channel->engine.resetHandler = ^{
      atomic_store(&channel->running, false);
      fprintf(stderr, "Core Haptics reset; session must restart\n");
      session.quitting = 1;
    };
    NSError *e = nil;
    if (![channel->engine startAndReturnError:&e]) {
        fprintf(stderr, "Engine start locality=%s error=%s\n", [locality UTF8String],
                e.description.UTF8String);
        return false;
    }
    atomic_store(&channel->running, true);
    CHHapticEventParameter *one =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                      value:1];
    CHHapticEvent *event =
        [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                      parameters:@[ one ]
                                    relativeTime:0
                                        duration:30];
    CHHapticDynamicParameter *zero = [[CHHapticDynamicParameter alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                      value:0
               relativeTime:0];
    channel->pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ]
                                                     parameters:@[ zero ]
                                                          error:&e];
    channel->player = [channel->engine createPlayerWithPattern:channel->pattern error:&e];
    if (!channel->player) {
        fprintf(stderr, "Player create locality=%s error=%s\n", [locality UTF8String],
                e.description.UTF8String);
        return false;
    }
    if (![channel->player startAtTime:CHHapticTimeImmediate error:&e]) {
        fprintf(stderr, "Player start locality=%s error=%s\n", [locality UTF8String],
                e.description.UTF8String);
        return false;
    }
    channel->level = 0;
    channel->started = CFAbsoluteTimeGetCurrent();
    return true;
}

bool DSBHapticSet(DSBHapticChannel *channel, float level) {
    level = fmaxf(0, fminf(1, level));
    if (fabsf(channel->level - level) < .001) {
        return true;
    }
    if (session.dry) {
        channel->level = level;
        return true;
    }
    if (!atomic_load(&channel->running)) {
        return level == 0;
    }
    NSError *error = nil;
    CHHapticDynamicParameter *p = [[CHHapticDynamicParameter alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                      value:level
               relativeTime:0];
    @try {
        if (![channel->player sendParameters:@[ p ] atTime:CHHapticTimeImmediate error:&error]) {
            fprintf(stderr, "Core Haptics update failed: %s\n", error.description.UTF8String);
            return false;
        }
    } @catch (NSException *exception) {
        atomic_store(&channel->running, false);
        fprintf(stderr, "Core Haptics became unavailable: %s\n", exception.reason.UTF8String);
        return false;
    }
    channel->level = level;
    return true;
}

bool DSBHapticRenew(DSBHapticChannel *channel) {
    if (session.dry || !channel->engine || CFAbsoluteTimeGetCurrent() - channel->started < 25) {
        return true;
    }
    NSError *e = nil;
    id<CHHapticPatternPlayer> next = [channel->engine createPlayerWithPattern:channel->pattern
                                                                         error:&e];
    if (!next) {
        return false;
    }
    [channel->player stopAtTime:CHHapticTimeImmediate error:NULL];
    channel->player = next;
    if (![next startAtTime:CHHapticTimeImmediate error:&e]) {
        return false;
    }
    channel->level = -1;
    channel->started = CFAbsoluteTimeGetCurrent();
    return true;
}

void DSBHapticClose(DSBHapticChannel *channel) {
    // Disconnect can stop the engine before cleanup. PatternPlayer stop then
    // raises an ObjC exception rather than returning NSError on macOS 27.
    @try {
        if (atomic_load(&channel->running)) {
            [channel->player stopAtTime:CHHapticTimeImmediate error:NULL];
        }
        [channel->engine stopWithCompletionHandler:nil];
    } @catch (NSException *exception) {
        fprintf(stderr, "Core Haptics already stopped during cleanup: %s\n",
                exception.reason.UTF8String);
    }
    atomic_store(&channel->running, false);
}
