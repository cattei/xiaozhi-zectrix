#ifndef SLEEP_MANAGER_H
#define SLEEP_MANAGER_H

#include <cstdint>

// ---------------------------------------------------------------------------
// TEMPORARY STUB -- NOT A REAL IMPLEMENTATION
//
// The zectrix board code (custom_lcd_display.cc) calls sm_kick()/sm_set_busy()
// to delay system sleep while the e-paper panel is refreshing. Those helpers
// are not part of this repository upstream, so they are stubbed here as no-ops
// to keep the board buildable.
//
// Consequence: a display refresh will NOT postpone entering sleep mode. That is
// acceptable today because sleep only happens after 10 minutes of inactivity,
// long after any refresh completes, but it is not the intended behaviour.
//
// Please replace this file with the real sleep-manager implementation (or drop
// the sm_*() calls in custom_lcd_display.cc) before merging upstream.
// ---------------------------------------------------------------------------
enum class SleepBusySrc {
    Display,
    Audio,
    Network,
    Other,
};

inline void sm_kick(uint32_t kick_ms, const char* reason) {
    (void)kick_ms;
    (void)reason;
}

inline void sm_set_busy(SleepBusySrc src, bool busy) {
    (void)src;
    (void)busy;
}

#endif  // SLEEP_MANAGER_H
