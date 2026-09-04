#include "fcs_internal.hpp"
#include "fcs_recreate_policy.hpp"

namespace fcs::hook {
namespace {

bool HasExactPlainAck(UINT slot, LONG64 sequence, LONG64 ringId) {
    const LONG64 firstSequence = g_ipcState.ipc->slotAckSequence[slot];
    MemoryBarrier();
    const LONG64 ackRingId = g_ipcState.ipc->slotAckRingId[slot];
    MemoryBarrier();
    const LONG64 secondSequence = g_ipcState.ipc->slotAckSequence[slot];
    return firstSequence == sequence && secondSequence == sequence &&
           ackRingId == ringId;
}

bool PollPlainProducerQueries(const LARGE_INTEGER& now) {
    if (g_captureState.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE) return true;

    const LONG64 currentRingId = g_ipcState.ipc->resourceRequestId;

    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (g_captureState.plainSlotStates[slot] == PLAIN_SLOT_PUBLISHED &&
            HasExactPlainAck(slot, g_captureState.plainSlotSequences[slot],
                             g_captureState.plainSlotRingIds[slot])) {
            g_captureState.plainSlotStates[slot] = PLAIN_SLOT_FREE;
        }
    }

    bool completed[FCS_SLOT_COUNT]{};
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (g_captureState.plainSlotStates[slot] != PLAIN_SLOT_COPY_PENDING) continue;
        const HRESULT hr = g_captureState.context->GetData(
            g_captureState.plainProducerQueries[slot], nullptr, 0,
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            if (g_captureState.plainSlotRingIds[slot] != currentRingId ||
                g_ipcState.ipc->sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE) {
                InvalidatePublishedResources();
                ReleaseCaptureResources();
                g_captureState.resourceRetryAfterQpc = now.QuadPart +
                    (g_captureState.qpcFrequency.QuadPart * 2);
                SetError(E_UNEXPECTED,
                         L"The compatibility frame ring changed while a GPU copy was pending; reconnecting.");
                return false;
            }
            completed[slot] = true;
        } else if (FAILED(hr)) {
            g_captureState.plainCreateError = hr;
            g_captureState.plainCreateStage = FCS_RESOURCE_STAGE_QUERY_COMPLETION;
            InvalidatePublishedResources();
            ReleaseCaptureResources();
            g_captureState.resourceRetryAfterQpc = now.QuadPart +
                (g_captureState.qpcFrequency.QuadPart * 2);
            SetError(hr,
                     L"The compatibility frame completion check failed; reconnecting.");
            return false;
        }
    }

    // Publish completed copies in sequence order so latestSlot/frameId never
    // move backwards if several event queries become ready on one Present.
    for (;;) {
        LONG chosen = -1;
        LONG64 chosenSequence = 0;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            if (!completed[slot]) continue;
            if (chosen < 0 || g_captureState.plainSlotSequences[slot] < chosenSequence) {
                chosen = static_cast<LONG>(slot);
                chosenSequence = g_captureState.plainSlotSequences[slot];
            }
        }
        if (chosen < 0) break;
        completed[chosen] = false;
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[chosen], chosenSequence);
        g_captureState.plainSlotStates[chosen] = PLAIN_SLOT_PUBLISHED;
        // A driver normally retires immediate-context event queries in order,
        // but preserve monotonic public metadata even if it reports them out
        // of order. The older slot remains visible for an exact stale ACK.
        if (chosenSequence > g_ipcState.ipc->frameId) {
            InterlockedExchange(&g_ipcState.ipc->latestSlot, chosen);
            InterlockedExchange64(&g_ipcState.ipc->qpcLastFrame,
                                  g_captureState.plainSlotQpc[chosen]);
            InterlockedExchange64(&g_ipcState.ipc->frameId, chosenSequence);
            if (g_ipcState.frameEvent) SetEvent(g_ipcState.frameEvent);
        }
    }
    return true;
}

} // namespace

PendingFrame TryCopyCleanFrame(IDXGISwapChain* swap) {
    PendingFrame result{};
    if (!g_ipcState.ipc || g_ipcState.ipc->stopRequested) return result;
    if (g_captureState.hostFallbackTerminal &&
        !(g_ipcState.ipc->command & FCS_COMMAND_RECREATE_RESOURCES)) return result;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    // One non-flushing completion poll per Present. This runs before the rate
    // gate so acknowledgements keep draining even when capture is throttled.
    if (!PollPlainProducerQueries(now)) return result;
    LONG fps = g_ipcState.ipc->captureFps;
    if (fps < 10) fps = 30;
    if (fps > 120) fps = 120;
    const LONG64 interval = g_captureState.qpcFrequency.QuadPart / fps;
    if (g_captureState.lastCaptureQpc && now.QuadPart - g_captureState.lastCaptureQpc < interval) {
        ++g_captureState.rateSkippedCounter;
        return result;
    }
    g_captureState.lastCaptureQpc = now.QuadPart;
    InterlockedExchange64(&g_ipcState.ipc->presentCount, g_captureState.presentCounter);
    InterlockedExchange64(&g_ipcState.ipc->rateSkipped, g_captureState.rateSkippedCounter);
    const bool coldRecreate = BypassResourceRetryForColdRecreate(
        (g_ipcState.ipc->command & FCS_COMMAND_RECREATE_RESOURCES) != 0,
        g_captureState.device != nullptr);
    if (now.QuadPart < g_captureState.resourceRetryAfterQpc &&
        !coldRecreate) {
        ++g_captureState.rateSkippedCounter;
        return result;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || !backBuffer) return result;

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    if (!EnsureCaptureResources(swap, desc)) {
        backBuffer->Release();
        return result;
    }

    LONG chosen = -1;
    bool mutexInvalid = false;
    const bool plainLegacy =
        g_captureState.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE;
    for (UINT attempt = 0; attempt < FCS_SLOT_COUNT; ++attempt) {
        const LONG slot = (g_captureState.nextSlot + static_cast<LONG>(attempt)) %
                          static_cast<LONG>(FCS_SLOT_COUNT);
        if (plainLegacy) {
            if (g_captureState.plainSlotStates[slot] == PLAIN_SLOT_FREE) {
                chosen = slot;
                break;
            }
        } else {
            hr = g_captureState.keyedMutexes[slot]->AcquireSync(0, 0);
            if (hr == S_OK) {
                chosen = slot;
                break;
            }
            if (hr != static_cast<HRESULT>(WAIT_TIMEOUT)) mutexInvalid = true;
        }
    }
    if (chosen < 0) {
        InterlockedIncrement64(&g_ipcState.ipc->busyDropped);
        backBuffer->Release();
        if (!plainLegacy && mutexInvalid) {
            InvalidatePublishedResources();
            ReleaseCaptureResources();
            g_captureState.resourceRetryAfterQpc = now.QuadPart + (g_captureState.qpcFrequency.QuadPart * 2);
            SetError(E_FAIL, L"The GPU sharing ring was abandoned; recreating it.");
        }
        return result;
    }

    LARGE_INTEGER cpuStart{};
    QueryPerformanceCounter(&cpuStart);
    if (plainLegacy) {
        // Retire the acknowledged frame before touching its texture again. A
        // controller viewer recreated within the same metadata generation can
        // therefore never mistake the old sequence for the in-flight copy.
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[chosen], 0);
        InterlockedCompareExchange(&g_ipcState.ipc->latestSlot, -1, chosen);
    }
    if (desc.SampleDesc.Count > 1) {
        g_captureState.context->ResolveSubresource(g_captureState.sharedTextures[chosen], 0, backBuffer, 0,
                                      g_captureState.transportFormat);
    } else {
        g_captureState.context->CopyResource(g_captureState.sharedTextures[chosen], backBuffer);
    }
    if (plainLegacy) {
        const LONG64 sequence = InterlockedIncrement64(&g_captureState.sequence);
        g_captureState.plainSlotSequences[chosen] = sequence;
        g_captureState.plainSlotRingIds[chosen] = g_ipcState.ipc->resourceRequestId;
        g_captureState.plainSlotQpc[chosen] = now.QuadPart;
        g_captureState.context->End(g_captureState.plainProducerQueries[chosen]);
        g_captureState.plainSlotStates[chosen] = PLAIN_SLOT_COPY_PENDING;
        InterlockedIncrement64(&g_ipcState.ipc->copiesSubmitted);
        hr = S_OK;
    } else {
        hr = g_captureState.keyedMutexes[chosen]->ReleaseSync(1);
    }
    backBuffer->Release();
    if (hr != S_OK) {
        InvalidatePublishedResources();
        ReleaseCaptureResources();
        g_captureState.resourceRetryAfterQpc = now.QuadPart + (g_captureState.qpcFrequency.QuadPart * 2);
        SetError(hr, L"The GPU sharing ring lost synchronization; recreating it.");
        return result;
    }

    LARGE_INTEGER cpuEnd{};
    QueryPerformanceCounter(&cpuEnd);
    const LONG64 elapsed = cpuEnd.QuadPart - cpuStart.QuadPart;
    LONG64 observed = g_ipcState.ipc->hookCpuTicksMax;
    while (elapsed > observed &&
           InterlockedCompareExchange64(&g_ipcState.ipc->hookCpuTicksMax, elapsed, observed) != observed) {
        observed = g_ipcState.ipc->hookCpuTicksMax;
    }

    g_captureState.nextSlot = (chosen + 1) % static_cast<LONG>(FCS_SLOT_COUNT);
    if (plainLegacy) return result;
    result.copied = true;
    result.slot = chosen;
    result.sequence = InterlockedIncrement64(&g_captureState.sequence);
    result.qpc = now.QuadPart;
    result.generation = g_ipcState.ipc->generation;
    return result;
}

void PublishFrame(const PendingFrame& frame) {
    if (!frame.copied || !g_ipcState.ipc || frame.generation != g_ipcState.ipc->generation) return;
    InterlockedExchange64(&g_ipcState.ipc->slotSequence[frame.slot], frame.sequence);
    InterlockedExchange(&g_ipcState.ipc->latestSlot, frame.slot);
    InterlockedExchange64(&g_ipcState.ipc->qpcLastFrame, frame.qpc);
    InterlockedExchange64(&g_ipcState.ipc->frameId, frame.sequence);
    InterlockedIncrement64(&g_ipcState.ipc->copiesSubmitted);
    if (g_ipcState.frameEvent) SetEvent(g_ipcState.frameEvent);
}

void HandleDeviceLoss(HRESULT hr, PendingFrame& pending) {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) return;
    pending.copied = false;
    InvalidatePublishedResources();
    ReleaseCaptureResources();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_captureState.resourceRetryAfterQpc = now.QuadPart + (g_captureState.qpcFrequency.QuadPart * 2);
    SetError(hr, L"FFXIV's graphics device was reset; waiting to reconnect.");
}

void ReleaseResourcesAfterHookOrderLoss() {
    if (!g_swapChainState.hookOrderLost || !g_captureState.device || !TryEnterCaptureGate()) return;
    if (g_captureState.device) {
        InvalidatePublishedResources();
        ReleaseCaptureResources();
    }
    LeaveCaptureGate();
}

} // namespace fcs::hook
