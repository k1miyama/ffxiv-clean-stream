#include "fcs_internal.hpp"

namespace fcs::hook {

bool TryEnterCaptureGate() {
    return InterlockedCompareExchange(&g_captureState.captureGate, 1, 0) == 0;
}

void LeaveCaptureGate() {
    InterlockedExchange(&g_captureState.captureGate, 0);
}

void SetMessage(const wchar_t* text) {
    if (!g_ipcState.ipc) return;
    lstrcpynW(g_ipcState.ipc->message, text, static_cast<int>(sizeof(g_ipcState.ipc->message) / sizeof(wchar_t)));
}

void SetError(LONG code, const wchar_t* text) {
    if (!g_ipcState.ipc) return;
    InterlockedExchange(&g_ipcState.ipc->lastError, code);
    SetMessage(text);
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_ERROR);
    if (g_ipcState.readyEvent) SetEvent(g_ipcState.readyEvent);
}

void BeginMetadataWrite() {
    LONG64 generation = InterlockedIncrement64(&g_ipcState.ipc->generation);
    if ((generation & 1) == 0) InterlockedIncrement64(&g_ipcState.ipc->generation);
    MemoryBarrier();
}

void EndMetadataWrite() {
    MemoryBarrier();
    LONG64 generation = InterlockedIncrement64(&g_ipcState.ipc->generation);
    if ((generation & 1) != 0) InterlockedIncrement64(&g_ipcState.ipc->generation);
    if (g_ipcState.readyEvent) SetEvent(g_ipcState.readyEvent);
}

void InvalidatePublishedResources() {
    if (!g_ipcState.ipc) return;
    BeginMetadataWrite();
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        g_ipcState.ipc->sharedNames[i][0] = L'\0';
        g_ipcState.ipc->sharedHandles[i] = 0;
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[i], 0);
    }
    g_ipcState.ipc->sharingMode = FCS_SHARING_NONE;
    g_ipcState.ipc->width = 0;
    g_ipcState.ipc->height = 0;
    g_ipcState.ipc->format = 0;
    EndMetadataWrite();
}

} // namespace fcs::hook
