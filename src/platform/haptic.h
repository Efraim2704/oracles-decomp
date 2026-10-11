#pragma once
// A short tick, the one the phone's keyboard gives, when a finger presses a control or taps a menu.
// Android only: it follows the system's touch feedback setting. Elsewhere nothing vibrates.
#include <stdbool.h>

#ifdef __ANDROID__
#define HAS_HAPTICS true
#else
#define HAS_HAPTICS false
#endif

void haptic_tick(void);
