#include "fcs_internal.hpp"

namespace fcs::hook {
namespace {

thread_local LONG g_proxyDepth = 0;

void MarkHookOrderLost() {
    if (!g_ipcState.ipc || InterlockedCompareExchange(&g_swapChainState.hookOrderLost, 1, 0) != 0) return;
    InterlockedExchange(&g_ipcState.ipc->lastError, ERROR_INVALID_STATE);
    SetMessage(L"MMOMinion changed the graphics hook after capture started. "
               L"Capture stopped safely to avoid showing the overlay or hurting FPS. "
               L"Restart FFXIV, wait for the MMOMinion GUI, then start this app again.");
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_HOOK_ORDER_LOST);
    if (g_ipcState.readyEvent) SetEvent(g_ipcState.readyEvent);
}

ProxyRecord* FindRecord(IDXGISwapChain* swap) {
    const LONG count = g_swapChainState.recordCount;
    void** current = *reinterpret_cast<void***>(swap);
    for (LONG i = count - 1; i >= 0; --i) {
        if (g_swapChainState.records[i].valid && g_swapChainState.records[i].swap == swap &&
            g_swapChainState.records[i].proxyVtable == current) {
            return &g_swapChainState.records[i];
        }
    }
    for (LONG i = count - 1; i >= 0; --i) {
        if (g_swapChainState.records[i].valid && g_swapChainState.records[i].swap == swap) {
            return &g_swapChainState.records[i];
        }
    }
    return nullptr;
}

LONG FindRecordIndex(IDXGISwapChain* swap) {
    const LONG count = g_swapChainState.recordCount;
    for (LONG i = count - 1; i >= 0; --i) {
        if (g_swapChainState.records[i].valid && g_swapChainState.records[i].swap == swap) return i;
    }
    return -1;
}

HRESULT STDMETHODCALLTYPE ProxyPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    const LONG depth = g_proxyDepth++;
    ProxyRecord* record = FindRecord(swap);
    PendingFrame pending{};
    void** current = *reinterpret_cast<void***>(swap);
    const bool ownsCurrentVtable = record && current == record->proxyVtable &&
                                   current[8] == reinterpret_cast<void*>(&ProxyPresent);
    const LONG latest = g_swapChainState.latestRecord;
    if (depth == 0 && record && latest >= 0 && !ownsCurrentVtable &&
        record == &g_swapChainState.records[latest]) {
        MarkHookOrderLost();
    }
    if (depth == 0 && !g_swapChainState.hookOrderLost && ownsCurrentVtable &&
        !(flags & DXGI_PRESENT_TEST) &&
        record && latest >= 0 &&
        record == &g_swapChainState.records[latest]) {
        if (TryEnterCaptureGate()) {
            if (g_ipcState.ipc->stopRequested) {
                if (g_captureState.device) {
                    InvalidatePublishedResources();
                    ReleaseCaptureResources();
                }
            } else {
                ++g_captureState.presentCounter;
                pending = TryCopyCleanFrame(swap);
                if (pending.copied) PublishFrame(pending);
            }
            LeaveCaptureGate();
        }
    }
    if (depth == 0 && g_swapChainState.hookOrderLost) ReleaseResourcesAfterHookOrderLoss();

    HRESULT hr = E_FAIL;
    if (depth == 0 && ownsCurrentVtable && record->nextPresent) {
        hr = record->nextPresent(swap, syncInterval, flags);
    } else if (g_swapChainState.bootstrapOriginal) {
        // A later hook may call our old proxy as its saved "original". Going
        // straight to the bootstrap trampoline avoids running the downstream
        // overlay a second time or recursing through a rehooked overlay.
        hr = g_swapChainState.bootstrapOriginal(swap, syncInterval, flags);
    }
    --g_proxyDepth;
    if (depth == 0 && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)) {
        pending.copied = false;
        if (TryEnterCaptureGate()) {
            HandleDeviceLoss(hr, pending);
            LeaveCaptureGate();
        } else {
            InterlockedExchange(&g_captureState.sourceChanged, 1);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE ProxyPresent1(IDXGISwapChain1* swap1, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    IDXGISwapChain* swap = static_cast<IDXGISwapChain*>(swap1);
    const LONG depth = g_proxyDepth++;
    ProxyRecord* record = FindRecord(swap);
    PendingFrame pending{};
    void** current = *reinterpret_cast<void***>(swap);
    const bool ownsCurrentVtable = record && current == record->proxyVtable &&
                                   current[22] == reinterpret_cast<void*>(&ProxyPresent1);
    const LONG latest = g_swapChainState.latestRecord;
    if (depth == 0 && record && latest >= 0 && !ownsCurrentVtable &&
        record == &g_swapChainState.records[latest]) {
        MarkHookOrderLost();
    }
    if (depth == 0 && !g_swapChainState.hookOrderLost && ownsCurrentVtable &&
        !(flags & DXGI_PRESENT_TEST) &&
        record && latest >= 0 &&
        record == &g_swapChainState.records[latest]) {
        if (TryEnterCaptureGate()) {
            if (g_ipcState.ipc->stopRequested) {
                if (g_captureState.device) {
                    InvalidatePublishedResources();
                    ReleaseCaptureResources();
                }
            } else {
                ++g_captureState.presentCounter;
                pending = TryCopyCleanFrame(swap);
                if (pending.copied) PublishFrame(pending);
            }
            LeaveCaptureGate();
        }
    }
    if (depth == 0 && g_swapChainState.hookOrderLost) ReleaseResourcesAfterHookOrderLoss();

    HRESULT hr = E_FAIL;
    if (depth == 0 && ownsCurrentVtable && record->nextPresent1) {
        hr = record->nextPresent1(swap1, syncInterval, flags, parameters);
    } else if (g_swapChainState.bootstrapOriginal1) {
        hr = g_swapChainState.bootstrapOriginal1(swap1, syncInterval, flags, parameters);
    } else if (g_swapChainState.bootstrapOriginal) {
        hr = g_swapChainState.bootstrapOriginal(swap, syncInterval, flags);
    }
    --g_proxyDepth;
    if (depth == 0 && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)) {
        pending.copied = false;
        if (TryEnterCaptureGate()) {
            HandleDeviceLoss(hr, pending);
            LeaveCaptureGate();
        } else {
            InterlockedExchange(&g_captureState.sourceChanged, 1);
        }
    }
    return hr;
}

size_t DetectVtableSlots(IDXGISwapChain* swap, size_t minimumSlots) {
    size_t slots = minimumSlots;
    IDXGISwapChain1* swap1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1),
                                       reinterpret_cast<void**>(&swap1)))) {
        if (reinterpret_cast<void*>(swap1) == reinterpret_cast<void*>(swap)) slots = kSwapChain1Slots;
        swap1->Release();
    }
    IDXGISwapChain2* swap2 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain2),
                                       reinterpret_cast<void**>(&swap2)))) {
        if (reinterpret_cast<void*>(swap2) == reinterpret_cast<void*>(swap)) slots = kSwapChain2Slots;
        swap2->Release();
    }
    IDXGISwapChain3* swap3 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3),
                                       reinterpret_cast<void**>(&swap3)))) {
        if (reinterpret_cast<void*>(swap3) == reinterpret_cast<void*>(swap)) slots = kSwapChain3Slots;
        swap3->Release();
    }
    IDXGISwapChain4* swap4 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain4),
                                       reinterpret_cast<void**>(&swap4)))) {
        if (reinterpret_cast<void*>(swap4) == reinterpret_cast<void*>(swap)) slots = kSwapChain4Slots;
        swap4->Release();
    }
    return slots;
}

bool WrapSwapChain(IDXGISwapChain* swap,
                   size_t minimumSlots = kSwapChainSlots) {
    if (!swap || !TryAcquireSRWLockExclusive(&g_swapChainState.wrapLock)) return false;
    bool wrapped = false;
    do {
        LONG count = g_swapChainState.recordCount;
        if (count >= kMaxProxyRecords) break;
        void** current = *reinterpret_cast<void***>(swap);
        if (!current) break;
        // Never stack another proxy on a swap chain we have already wrapped.
        // If a later overlay replaced our vtable, wrapping again would append
        // that same overlay to the forwarding chain once per attempt.
        if (FindRecordIndex(swap) >= 0) break;
        const size_t vtableSlots = DetectVtableSlots(swap, minimumSlots);
        const bool presentWrapped = current[8] == reinterpret_cast<void*>(&ProxyPresent);
        const bool present1Wrapped = vtableSlots <= 22 ||
            current[22] == reinterpret_cast<void*>(&ProxyPresent1);
        if (presentWrapped && present1Wrapped) break;

        void** clone = static_cast<void**>(VirtualAlloc(
            nullptr, vtableSlots * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!clone) break;
        memcpy(clone, current, vtableSlots * sizeof(void*));
        clone[8] = reinterpret_cast<void*>(&ProxyPresent);
        if (vtableSlots > 22) clone[22] = reinterpret_cast<void*>(&ProxyPresent1);

        ProxyRecord& record = g_swapChainState.records[count];
        record.swap = swap;
        record.proxyVtable = clone;
        record.nextPresent = reinterpret_cast<PresentFn>(current[8]);
        record.nextPresent1 = vtableSlots > 22 ? reinterpret_cast<Present1Fn>(current[22]) : nullptr;
        record.vtableSlots = vtableSlots;
        // State 2 is a short installation window. Readers may use the fully
        // initialized record for forwarding, but Bootstrap must not mistake
        // the not-yet-swapped vtable for a later third-party rehook.
        InterlockedExchange(&record.valid, 2);
        MemoryBarrier();
        InterlockedExchange(&g_swapChainState.recordCount, count + 1);

        void* previous = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(swap), clone, current);
        if (previous != current) {
            InterlockedExchange(&record.valid, 0);
            break;
        }
        InterlockedExchange(&g_swapChainState.latestRecord, count);
        InterlockedExchange(&record.valid, 1);
        if (g_ipcState.ipc) {
            InterlockedIncrement64(&g_ipcState.ipc->rewrapCount);
            SetMessage(L"Present is wrapped before the MMOMinion overlay.");
            InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_HOOKED);
        }
        wrapped = true;
    } while (false);
    ReleaseSRWLockExclusive(&g_swapChainState.wrapLock);
    return wrapped;
}

bool IsTargetSwapChain(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) return false;
    const HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(g_ipcState.ipc->targetHwnd));
    if (!target || desc.OutputWindow != target) return false;
    DWORD owner = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &owner);
    if (owner != GetCurrentProcessId()) return false;
    ID3D11Device* device = nullptr;
    const HRESULT hr = swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    ReleaseCom(device);
    return SUCCEEDED(hr);
}

} // namespace

HRESULT STDMETHODCALLTYPE BootstrapPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    if (g_proxyDepth > 0) return g_swapChainState.bootstrapOriginal(swap, syncInterval, flags);

    if (g_ipcState.ipc) InterlockedIncrement64(&g_ipcState.ipc->bootstrapPresentCount);
    const bool targetSwap = g_ipcState.ipc && IsTargetSwapChain(swap);
    if (g_ipcState.ipc && !targetSwap) InterlockedIncrement64(&g_ipcState.ipc->targetRejectedPresentCount);
    if (g_ipcState.ipc && !g_ipcState.ipc->stopRequested && !(flags & DXGI_PRESENT_TEST) && targetSwap) {
        const LONG existing = FindRecordIndex(swap);
        if (existing >= 0) {
            if (g_swapChainState.records[existing].valid == 1 && existing == g_swapChainState.latestRecord) {
                MarkHookOrderLost();
            }
        } else if (g_swapChainState.candidate == swap) {
            ++g_swapChainState.candidatePresents;
        } else {
            g_swapChainState.candidate = swap;
            g_swapChainState.candidatePresents = 1;
        }
        if (existing < 0 && g_swapChainState.candidatePresents >= 3) WrapSwapChain(swap);
    }
    return g_swapChainState.bootstrapOriginal(swap, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE BootstrapPresent1(IDXGISwapChain1* swap1, UINT syncInterval, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* parameters) {
    if (g_proxyDepth > 0) {
        return g_swapChainState.bootstrapOriginal1(swap1, syncInterval, flags, parameters);
    }
    IDXGISwapChain* swap = static_cast<IDXGISwapChain*>(swap1);
    if (g_ipcState.ipc) InterlockedIncrement64(&g_ipcState.ipc->bootstrapPresentCount);
    const bool targetSwap = g_ipcState.ipc && IsTargetSwapChain(swap);
    if (g_ipcState.ipc && !targetSwap) InterlockedIncrement64(&g_ipcState.ipc->targetRejectedPresentCount);
    if (g_ipcState.ipc && !g_ipcState.ipc->stopRequested && !(flags & DXGI_PRESENT_TEST) && targetSwap) {
        const LONG existing = FindRecordIndex(swap);
        if (existing >= 0) {
            if (g_swapChainState.records[existing].valid == 1 && existing == g_swapChainState.latestRecord) {
                MarkHookOrderLost();
            }
        } else if (g_swapChainState.candidate == swap) {
            ++g_swapChainState.candidatePresents;
        } else {
            g_swapChainState.candidate = swap;
            g_swapChainState.candidatePresents = 1;
        }
        if (existing < 0 && g_swapChainState.candidatePresents >= 3) WrapSwapChain(swap, kSwapChain1Slots);
    }
    return g_swapChainState.bootstrapOriginal1(swap1, syncInterval, flags, parameters);
}

} // namespace fcs::hook
