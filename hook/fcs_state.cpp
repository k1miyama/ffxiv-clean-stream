#include "fcs_internal.hpp"

namespace fcs::hook {

// Trivial aggregate definitions guarantee loader-safe static zero
// initialization; no constructors run while Windows holds the loader lock.
HookProcessState g_processState{};
IpcRuntimeState g_ipcState{};
SwapChainRuntimeState g_swapChainState{};
CaptureRuntimeState g_captureState{};

void InitializeRuntimeState(HMODULE module) {
    ZeroMemory(&g_processState, sizeof(g_processState));
    ZeroMemory(&g_ipcState, sizeof(g_ipcState));
    ZeroMemory(&g_swapChainState, sizeof(g_swapChainState));
    ZeroMemory(&g_captureState, sizeof(g_captureState));

    g_processState.module = module;
    g_swapChainState.latestRecord = -1;
    g_captureState.sourceSamples = 1;
    g_captureState.sharingMode = FCS_SHARING_NONE;
    g_captureState.requestedHostMode = FCS_SHARING_NONE;
    g_captureState.lastHostAttemptGeneration = -1;
    g_captureState.plainCreateError = E_PENDING;
    g_captureState.hostNtNamePathError = E_PENDING;
    g_captureState.hostNtHandlePathError = E_PENDING;
    g_captureState.hostLegacyPathError = E_PENDING;
}

} // namespace fcs::hook
