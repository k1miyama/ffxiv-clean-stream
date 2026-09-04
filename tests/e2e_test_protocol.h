#pragma once

#include <windows.h>
#include <stdint.h>

static constexpr uint32_t FCS_TEST_MAGIC = 0x31545346u; // "FST1"

enum FcsTestGameState : LONG {
    FCS_TEST_GAME_STARTING = 0,
    FCS_TEST_GAME_READY = 1,
    FCS_TEST_GAME_ERROR = -1,
};

// Test-only telemetry from the hidden synthetic game. The production capture
// helper never reads this mapping; it exists so the controller can prove that
// the downstream red overlay actually executed.
struct alignas(64) FcsTestGameIpc {
    uint32_t magic;
    uint32_t bytes;
    volatile LONG state;
    volatile LONG stopRequested;
    volatile LONG64 targetHwnd;
    volatile LONG64 gameFrames;
    volatile LONG64 overlayRuns;
    volatile LONG resizeRequested;
    volatile LONG resizeCompleted;
    volatile LONG resizedWidth;
    volatile LONG resizedHeight;
    // The controller sets lateRehookRequested only after the production
    // helper has installed its per-swap-chain proxy. The synthetic game then
    // places a mock overlay vtable above that proxy. Before every later game
    // Present it reapplies the mock only if another component overwrote it.
    // This models an overlay watchdog and catches proxy-chain amplification.
    volatile LONG lateRehookRequested;
    volatile LONG lateRehookCompleted;
    volatile LONG lateOverlayInstalls;
    volatile LONG lateOverlayMaxCallsPerPresent;
    volatile LONG64 latePresentFrames;
    volatile LONG64 lateOverlayRuns;
    volatile LONG64 lateOverlayDuplicateFrames;
    volatile LONG64 lateOverlayMissingFrames;
    wchar_t message[128];
};

inline void FcsTestGameMappingName(wchar_t* output, DWORD pid) {
    wsprintfW(output, L"Local\\FCS1.TestGame.%lu", static_cast<unsigned long>(pid));
}

inline void FcsTestGameReadyEventName(wchar_t* output, DWORD pid) {
    wsprintfW(output, L"Local\\FCS1.TestGameReady.%lu", static_cast<unsigned long>(pid));
}
