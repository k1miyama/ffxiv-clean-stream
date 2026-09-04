// End-to-end regression for the final producer-owned, non-keyed legacy
// sharing fallback. This exercises the production injected DLL against the
// hidden synthetic game; it never opens FFXIV, Discord, or MMOMinion.
#include "capture_e2e_support.hpp"

#include <stdio.h>

using namespace fcs_test;

namespace {

constexpr DWORD kQueryTimeoutMilliseconds = 5000;
constexpr LONG64 kLateSettleFrames = 2;
constexpr LONG64 kLateObservedFrames = 8;

void CommitPlainAck(FcsIpcV1* ipc, LONG64 ringId, UINT slot,
                    LONG64 sequence, bool& committed) {
    committed = false;
    if (!ipc || slot >= FCS_SLOT_COUNT || sequence <= 0) return;

    // The stable ring ID, sharing mode, and exact sequence are the ACK
    // identity. The metadata seqlock may change without replacing resources.
    if (ipc->sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE ||
        ipc->resourceRequestId != ringId ||
        ipc->slotSequence[slot] != sequence) return;

    // Sequence is the commit field. This is the same publication order used
    // by the production controller.
    InterlockedExchange64(&ipc->slotAckRingId[slot], ringId);
    MemoryBarrier();
    if (ipc->sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE ||
        ipc->resourceRequestId != ringId ||
        ipc->slotSequence[slot] != sequence) return;
    InterlockedExchange64(&ipc->slotAckSequence[slot], sequence);
    committed = true;
}

struct PlainFrameConsumer {
    HWND window = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* preview = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Texture2D* shared[FCS_SLOT_COUNT]{};
    ID3D11Query* completion = nullptr;
    LONG64 consumedSequence[FCS_SLOT_COUNT]{};
    bool pending = false;
    LONG pendingSlot = -1;
    LONG64 pendingSequence = 0;
    LONG64 pendingRingId = 0;

    ~PlainFrameConsumer() { Reset(); }

    void Reset() {
        pending = false;
        pendingSlot = -1;
        pendingSequence = 0;
        pendingRingId = 0;
        ReleaseCom(completion);
        ReleaseCom(staging);
        ReleaseCom(preview);
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            ReleaseCom(shared[slot]);
            consumedSequence[slot] = 0;
        }
        ReleaseCom(swap);
        ReleaseCom(context);
        ReleaseCom(device);
        if (window) {
            DestroyWindow(window);
            window = nullptr;
        }
    }

    bool Initialize(const StableMetadata& metadata) {
        Reset();
        if (metadata.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE) return false;

        window = CreateWindowExW(0, L"STATIC", L"FCS plain-ring test sink",
                                 WS_POPUP, 0, 0,
                                 static_cast<int>(metadata.width),
                                 static_cast<int>(metadata.height),
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return false;

        IDXGIAdapter1* adapter = FindAdapter(metadata.adapterLuid);
        if (!adapter) return false;
        HRESULT hr = D3D11CreateDevice(
            adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context);
        if (FAILED(hr)) {
            adapter->Release();
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        hr = adapter->GetParent(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void**>(&factory));
        adapter->Release();
        if (FAILED(hr) || !factory) return false;

        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferDesc.Width = metadata.width;
        swapDesc.BufferDesc.Height = metadata.height;
        swapDesc.BufferDesc.Format = metadata.format;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.OutputWindow = window;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(device, &swapDesc, &swap);
        factory->Release();
        if (SUCCEEDED(hr)) {
            hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&preview));
        }

        for (UINT slot = 0; SUCCEEDED(hr) && slot < FCS_SLOT_COUNT; ++slot) {
            hr = OpenSharedTexture(device, nullptr, metadata, slot, &shared[slot]);
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC opened{};
                shared[slot]->GetDesc(&opened);
                if ((opened.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0 ||
                    (opened.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0 ||
                    opened.Width != metadata.width || opened.Height != metadata.height ||
                    opened.Format != metadata.format) {
                    hr = E_INVALIDARG;
                }
            }
        }

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = metadata.width;
        stagingDesc.Height = metadata.height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = metadata.format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) {
            hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
        }

        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_EVENT;
        if (SUCCEEDED(hr)) hr = device->CreateQuery(&queryDesc, &completion);
        if (FAILED(hr)) {
            wprintf(L"FAIL: initializing plain-ring consumer (0x%08lx)\n",
                    static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    bool BeginNewest(FcsIpcV1* ipc, const StableMetadata& metadata) {
        if (!ipc || pending || metadata.generation != ipc->generation) return false;
        LONG newestSlot = -1;
        LONG64 newestSequence = 0;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = ipc->slotSequence[slot];
            if (sequence > consumedSequence[slot] && sequence > newestSequence) {
                newestSlot = static_cast<LONG>(slot);
                newestSequence = sequence;
            }
        }
        if (newestSlot < 0) return false;

        // Frames older than the one selected were never read, so they may be
        // acknowledged immediately. The selected slot is held until the
        // consumer GPU event proves its CopyResource has completed.
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = ipc->slotSequence[slot];
            if (static_cast<LONG>(slot) == newestSlot ||
                sequence <= consumedSequence[slot]) continue;
            bool committed = false;
            CommitPlainAck(ipc, metadata.resourceRequestId, slot, sequence,
                           committed);
            if (committed) consumedSequence[slot] = sequence;
        }

        context->CopyResource(preview, shared[newestSlot]);
        context->End(completion);
        const HRESULT present = swap->Present(0, 0);
        if (FAILED(present)) return false;
        pending = true;
        pendingSlot = newestSlot;
        pendingSequence = newestSequence;
        pendingRingId = metadata.resourceRequestId;
        return true;
    }

    // Returns true once the GPU event has completed. ackCommitted separately
    // reports whether the exact generation/sequence was still current.
    bool PollComplete(FcsIpcV1* ipc, bool verifyPixels,
                      bool& ackCommitted, bool& pixelsClean) {
        ackCommitted = false;
        pixelsClean = false;
        if (!pending) return false;
        BOOL complete = FALSE;
        const HRESULT query = context->GetData(
            completion, &complete, sizeof(complete),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (query == S_FALSE) return false;
        if (FAILED(query) || !complete) {
            pending = false;
            return true;
        }

        if (verifyPixels) {
            context->CopyResource(staging, preview);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT map = context->Map(
                staging, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(map)) {
                auto pixelAt = [&](UINT x, UINT y) -> const uint8_t* {
                    return static_cast<const uint8_t*>(mapped.pData) +
                           static_cast<size_t>(y) * mapped.RowPitch + x * 4;
                };
                const uint8_t* scene = pixelAt(2, 2);
                const uint8_t* hud = pixelAt(12, 12);
                const uint8_t* overlay = pixelAt(52, 12);
                uint32_t redPixels = 0;
                D3D11_TEXTURE2D_DESC desc{};
                staging->GetDesc(&desc);
                for (UINT y = 0; y < desc.Height; ++y) {
                    for (UINT x = 0; x < desc.Width; ++x) {
                        const uint8_t* pixel = pixelAt(x, y);
                        if (pixel[0] > 239 && pixel[1] < 16 && pixel[2] < 16) {
                            ++redPixels;
                        }
                    }
                }
                pixelsClean = scene[0] < 16 && scene[1] < 16 && scene[2] > 239 &&
                              hud[0] < 16 && hud[1] > 239 && hud[2] < 16 &&
                              overlay[0] < 16 && overlay[1] < 16 && overlay[2] > 239 &&
                              redPixels == 0;
                context->Unmap(staging, 0);
            }
        } else {
            pixelsClean = true;
        }

        CommitPlainAck(ipc, pendingRingId, static_cast<UINT>(pendingSlot),
                       pendingSequence, ackCommitted);
        if (ackCommitted) consumedSequence[pendingSlot] = pendingSequence;
        pending = false;
        pendingSlot = -1;
        return true;
    }

    bool WaitComplete(FcsIpcV1* ipc, bool verifyPixels,
                      bool& ackCommitted, bool& pixelsClean) {
        const ULONGLONG deadline = GetTickCount64() + kQueryTimeoutMilliseconds;
        while (GetTickCount64() < deadline) {
            if (PollComplete(ipc, verifyPixels, ackCommitted, pixelsClean)) return true;
            Sleep(1);
        }
        return false;
    }

    void Drain(FcsIpcV1* ipc, const StableMetadata& metadata) {
        if (pending) {
            bool acked = false;
            bool ignored = false;
            PollComplete(ipc, false, acked, ignored);
        }
        if (!pending) BeginNewest(ipc, metadata);
    }
};

bool WaitForFullPlainRing(FcsIpcV1* ipc, HANDLE frameEvent) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        bool full = ipc->sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            if (ipc->slotSequence[slot] <= 0 || ipc->slotAckSequence[slot] != 0) {
                full = false;
            }
        }
        if (full) return true;
        WaitForSingleObject(frameEvent, 10);
    }
    return false;
}

bool WaitForLateFramesPlain(FcsTestGameIpc* testIpc, FcsIpcV1* captureIpc,
                            PlainFrameConsumer& consumer,
                            const StableMetadata& metadata, LONG64 target,
                            DWORD timeoutMilliseconds) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    while (GetTickCount64() < deadline) {
        consumer.Drain(captureIpc, metadata);
        if (testIpc->state == FCS_TEST_GAME_ERROR) return false;
        if (testIpc->lateRehookCompleted &&
            testIpc->latePresentFrames >= target) return true;
        Sleep(1);
    }
    return false;
}

} // namespace

int main() {
    PROCESS_INFORMATION child{};
    HANDLE testMapping = nullptr;
    HANDLE testReadyEvent = nullptr;
    FcsTestGameIpc* testIpc = nullptr;
    HANDLE captureMapping = nullptr;
    HANDLE captureReadyEvent = nullptr;
    HANDLE captureFrameEvent = nullptr;
    FcsIpcV1* captureIpc = nullptr;
    bool passed = false;

    do {
        if (!LaunchHiddenSyntheticGame(child) ||
            !OpenSyntheticTelemetry(child.dwProcessId, testMapping,
                                    testReadyEvent, testIpc)) {
            wprintf(L"FAIL: hidden synthetic target did not become ready\n");
            break;
        }

        const HWND target = reinterpret_cast<HWND>(
            static_cast<uintptr_t>(testIpc->targetHwnd));
        if (!CreateCaptureSession(child.dwProcessId, target, captureMapping,
                                  captureReadyEvent, captureFrameEvent, captureIpc)) {
            wprintf(L"FAIL: could not create production IPC objects\n");
            break;
        }
        InterlockedExchange(&captureIpc->captureFps, 60);
        InterlockedOr(&captureIpc->command,
                      FCS_COMMAND_FORCE_HOST_RESOURCES |
                      FCS_COMMAND_TEST_REJECT_HOST_NT |
                      FCS_COMMAND_TEST_REJECT_HOST_LEGACY);

        wchar_t hookPath[MAX_PATH]{};
        if (!BuildSiblingPath(hookPath, MAX_PATH, L"FfxivCleanStreamHook64.dll") ||
            GetFileAttributesW(hookPath) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"FAIL: current production hook DLL is missing beside this test\n");
            break;
        }
        if (!InjectHook(child.dwProcessId, hookPath) ||
            !WaitForCleanFrame(captureIpc, captureFrameEvent)) break;

        StableMetadata initial{};
        if (!ReadStableMetadata(captureIpc, initial) ||
            initial.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE) {
            wprintf(L"FAIL: final plain legacy sharing path was not reached (mode=%u)\n",
                    static_cast<unsigned int>(captureIpc->sharingMode));
            break;
        }
        if (!SawHostNtRequest() || !SawHostLegacyRequest() ||
            ForcedHostRequestCount() != 2 ||
            !FAILED(static_cast<HRESULT>(captureIpc->hostNtNamePathError)) ||
            !FAILED(static_cast<HRESULT>(captureIpc->hostNtHandlePathError)) ||
            !FAILED(static_cast<HRESULT>(captureIpc->hostLegacyPathError)) ||
            FAILED(static_cast<HRESULT>(captureIpc->producerPlainError))) {
            wprintf(L"FAIL: fallback traversal diagnostics were incomplete\n");
            break;
        }

        // Do not consume initially. All three slots must fill, then the helper
        // must drop without waiting while the hidden game keeps rendering.
        if (!WaitForFullPlainRing(captureIpc, captureFrameEvent)) {
            wprintf(L"FAIL: plain ring did not fill without a consumer\n");
            break;
        }
        const LONG64 heldCopies = captureIpc->copiesSubmitted;
        const LONG64 heldDrops = captureIpc->busyDropped;
        const LONG64 heldGameFrames = testIpc->gameFrames;
        Sleep(350);
        const bool slowConsumerSafe =
            captureIpc->copiesSubmitted == heldCopies &&
            captureIpc->busyDropped > heldDrops &&
            testIpc->gameFrames > heldGameFrames + 5;
        if (!slowConsumerSafe) {
            wprintf(L"FAIL: full ring stalled or overwrote (copies=%lld->%lld, "
                    L"drops=%lld->%lld, game=%lld->%lld)\n",
                    heldCopies, captureIpc->copiesSubmitted,
                    heldDrops, captureIpc->busyDropped,
                    heldGameFrames, testIpc->gameFrames);
            break;
        }

        PlainFrameConsumer consumer;
        if (!consumer.Initialize(initial) ||
            !consumer.BeginNewest(captureIpc, initial)) {
            wprintf(L"FAIL: could not begin nonblocking plain-ring read\n");
            break;
        }
        bool initialAck = false;
        bool initialPixels = false;
        if (!consumer.WaitComplete(captureIpc, true, initialAck, initialPixels) ||
            !initialAck || !initialPixels) {
            wprintf(L"FAIL: initial GPU-query read did not ACK a clean frame\n");
            break;
        }

        const LONG64 copiesBeforeDrain = captureIpc->copiesSubmitted;
        const ULONGLONG drainDeadline = GetTickCount64() + 1500;
        while (GetTickCount64() < drainDeadline &&
               captureIpc->copiesSubmitted <= copiesBeforeDrain + 3) {
            consumer.Drain(captureIpc, initial);
            WaitForSingleObject(captureFrameEvent, 2);
        }
        if (captureIpc->copiesSubmitted <= copiesBeforeDrain + 3) {
            wprintf(L"FAIL: exact ACKs did not restart the producer ring\n");
            break;
        }

        // Start a real consumer copy but deliberately withhold its completed
        // ACK until after ResizeBuffers installs a different generation.
        if (consumer.pending) {
            bool priorAck = false;
            bool ignored = false;
            if (!consumer.WaitComplete(captureIpc, false, priorAck, ignored) ||
                !priorAck) {
                wprintf(L"FAIL: could not finish the prior consumer query\n");
                break;
            }
        }
        const ULONGLONG pendingDeadline = GetTickCount64() + 3000;
        while (!consumer.BeginNewest(captureIpc, initial) &&
               GetTickCount64() < pendingDeadline) {
            WaitForSingleObject(captureFrameEvent, 2);
        }
        if (!consumer.pending) {
            wprintf(L"FAIL: could not hold an old-generation consumer query\n");
            break;
        }

        const LONG64 oldGeneration = initial.generation;
        const LONG64 oldFrameId = captureIpc->frameId;
        if (!RequestResizeAndWaitForNewFrame(testIpc, captureIpc, captureFrameEvent,
                                             oldGeneration, oldFrameId)) break;
        StableMetadata resized{};
        if (!ReadStableMetadata(captureIpc, resized) ||
            resized.generation <= oldGeneration ||
            resized.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE ||
            resized.width != 128 || resized.height != 80) {
            wprintf(L"FAIL: resize did not replace the plain shared ring\n");
            break;
        }

        LONG64 ackSequenceBefore[FCS_SLOT_COUNT]{};
        LONG64 ackRingIdBefore[FCS_SLOT_COUNT]{};
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            ackSequenceBefore[slot] = captureIpc->slotAckSequence[slot];
            ackRingIdBefore[slot] = captureIpc->slotAckRingId[slot];
        }
        bool staleAck = false;
        bool ignoredPixels = false;
        if (!consumer.WaitComplete(captureIpc, false, staleAck, ignoredPixels) ||
            staleAck) {
            wprintf(L"FAIL: old-generation query was accepted after resize\n");
            break;
        }
        bool ackFieldsUnchanged = true;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            if (captureIpc->slotAckSequence[slot] != ackSequenceBefore[slot] ||
                captureIpc->slotAckRingId[slot] != ackRingIdBefore[slot]) {
                ackFieldsUnchanged = false;
            }
        }
        if (!ackFieldsUnchanged) {
            wprintf(L"FAIL: stale completion modified the replacement ring ACKs\n");
            break;
        }

        if (!consumer.Initialize(resized) ||
            !consumer.BeginNewest(captureIpc, resized)) {
            wprintf(L"FAIL: could not open resized plain shared ring\n");
            break;
        }
        bool resizedAck = false;
        bool resizedPixels = false;
        if (!consumer.WaitComplete(captureIpc, true, resizedAck, resizedPixels) ||
            !resizedAck || !resizedPixels) {
            wprintf(L"FAIL: resized GPU-query read was not clean\n");
            break;
        }

        // Keep draining during the late-hook probe so a full ring cannot make
        // an unsafe implementation appear to have failed closed.
        const LONG64 liveCopies = captureIpc->copiesSubmitted;
        const ULONGLONG liveDeadline = GetTickCount64() + 3000;
        while (captureIpc->copiesSubmitted <= liveCopies &&
               GetTickCount64() < liveDeadline) {
            consumer.Drain(captureIpc, resized);
            Sleep(1);
        }
        if (captureIpc->copiesSubmitted <= liveCopies) {
            wprintf(L"FAIL: resized plain ring was not live before late hook\n");
            break;
        }

        InterlockedExchange(&testIpc->lateRehookRequested, 1);
        if (!WaitForLateFramesPlain(testIpc, captureIpc, consumer, resized,
                                    kLateSettleFrames, 5000)) {
            wprintf(L"FAIL: late overlay did not settle\n");
            break;
        }
        const LONG64 frameAfterSettle = captureIpc->frameId;
        const LONG64 copiesAfterSettle = captureIpc->copiesSubmitted;
        const LONG64 lateAfterSettle = testIpc->latePresentFrames;
        const LONG stateAfterSettle = captureIpc->state;
        if (!WaitForLateFramesPlain(testIpc, captureIpc, consumer, resized,
                                    lateAfterSettle + kLateObservedFrames, 5000)) {
            wprintf(L"FAIL: late overlay observation timed out\n");
            break;
        }

        // Freeze the test telemetry before comparing paired counters. Without
        // this, the hidden game can advance one side between the assertion
        // and diagnostic print even though every completed frame was correct.
        InterlockedExchange(&testIpc->stopRequested, 1);
        if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
            wprintf(L"FAIL: synthetic child did not stop after the bounded probe\n");
            break;
        }
        const LONG64 finalLateRuns = testIpc->lateOverlayRuns;
        const LONG64 finalLateFrames = testIpc->latePresentFrames;

        const bool exactlyOnce =
            finalLateRuns == finalLateFrames &&
            testIpc->lateOverlayDuplicateFrames == 0 &&
            testIpc->lateOverlayMissingFrames == 0 &&
            testIpc->lateOverlayMaxCallsPerPresent == 1 &&
            testIpc->lateOverlayInstalls == 1;
        const bool captureStopped =
            captureIpc->frameId == frameAfterSettle &&
            captureIpc->copiesSubmitted == copiesAfterSettle &&
            stateAfterSettle == FCS_STATE_HOOK_ORDER_LOST;
        const bool captureResourcesReleased =
            captureIpc->sharingMode == FCS_SHARING_NONE &&
            captureIpc->width == 0 && captureIpc->height == 0;
        if (!exactlyOnce || !captureStopped || !captureResourcesReleased) {
            wprintf(L"FAIL: late hook was amplified or plain capture did not fail closed\n");
            break;
        }

        wprintf(L"PASS: plain legacy query/ACK ring is nonblocking, survives resize, "
                L"rejects stale ACKs, excludes overlay pixels, and fails closed\n");
        wprintf(L"Slow-consumer evidence: game=%lld frames, copies=%lld, drops=%lld; "
                L"late-overlay calls=%lld/%lld\n",
                testIpc->gameFrames, captureIpc->copiesSubmitted,
                captureIpc->busyDropped, finalLateRuns, finalLateFrames);
        passed = true;
    } while (false);

    if (captureIpc) InterlockedExchange(&captureIpc->stopRequested, 1);
    if (testIpc) InterlockedExchange(&testIpc->stopRequested, 1);
    if (child.hProcess) {
        if (WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) {
            // This is only the hidden child launched by this bounded test.
            TerminateProcess(child.hProcess, 2);
            WaitForSingleObject(child.hProcess, 1000);
        }
    }
    DestroyForcedHostRing();
    if (captureIpc) UnmapViewOfFile(captureIpc);
    if (captureFrameEvent) CloseHandle(captureFrameEvent);
    if (captureReadyEvent) CloseHandle(captureReadyEvent);
    if (captureMapping) CloseHandle(captureMapping);
    if (testIpc) UnmapViewOfFile(testIpc);
    if (testReadyEvent) CloseHandle(testReadyEvent);
    if (testMapping) CloseHandle(testMapping);
    if (child.hThread) CloseHandle(child.hThread);
    if (child.hProcess) CloseHandle(child.hProcess);
    return passed ? 0 : 1;
}
