#pragma once
// One Core Haptics channel on a pad macOS knows (GCController.haptics): an engine
// on one locality, playing a continuous event whose intensity follows the level
// set. The DualSense backend uses two, on the handles, when it runs without PCM;
// the GameController backend uses the handles and, where the pad has them, the
// triggers.
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    CHHapticEngine *engine;
    id<CHHapticPatternPlayer> player;
    CHHapticPattern *pattern;
    atomic_bool running;
    float level;    // the last level played; -1 before the first
    double started; // when the player started; a new one takes over after 25 s
} DSBHapticChannel;

// Start a channel on `locality`. False, with a log line, when the pad has no
// engine there or it does not start. An engine that stops or resets later ends
// the session.
bool DSBHapticOpen(DSBHapticChannel *channel, GCController *pad, GCHapticsLocality locality);
// The intensity, 0 to 1. A dry run only records it. A channel that is not
// running accepts only 0.
bool DSBHapticSet(DSBHapticChannel *channel, float level);
// The continuous event lasts 30 s, so every 25 s a new player takes over.
bool DSBHapticRenew(DSBHapticChannel *channel);
void DSBHapticClose(DSBHapticChannel *channel);
