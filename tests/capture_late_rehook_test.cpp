// Regression for an overlay that replaces the swap-chain vtable after the
// production helper has already installed its clean-capture proxy.
#include "capture_e2e_support.hpp"

#include <stdio.h>

using namespace fcs_test;

namespace {

constexpr LONG64 kSettleLateFrames = 2;
constexpr LONG64 kObservedLateFrames = 8;

struct SlotDrainer {
    ID3D11Device* device = nullptr;
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* shared[FCS_SLOT_COUNT]{};
    IDXGIKeyedMutex* mutexes[FCS_SLOT_COUNT]{};
    LONG64 consumedSequence[FCS_SLOT_COUNT]{};

    ~SlotDrainer() {
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            ReleaseCom(mutexes[slot]);
            ReleaseCom(shared[slot]);
        }
        ReleaseCom(context);
        ReleaseCom(device1);
        ReleaseCom(device);
    }

    bool Initialize(const StableMetadata& metadata) {
        IDXGIAdapter1* adapter = FindAdapter(metadata.adapterLuid);
        if (!adapter) return false;
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &device,
                                       nullptr, &context);
        adapter->Release();
        if (FAILED(hr)) return false;
        hr = device->QueryInterface(__uuidof(ID3D11Device1),
                                    reinterpret_cast<void**>(&device1));
        if (FAILED(hr) && metadata.sharingMode == FCS_SHARING_NT_NAME) return false;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            hr = OpenSharedTexture(device, device1, metadata, slot, &shared[slot]);
            if (SUCCEEDED(hr)) {
                hr = shared[slot]->QueryInterface(
                    __uuidof(IDXGIKeyedMutex),
                    reinterpret_cast<void**>(&mutexes[slot]));
            }
            if (FAILED(hr)) return false;
        }
        return true;
    }

    void Drain(FcsIpcV1* ipc) {
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = ipc->slotSequence[slot];
            if (sequence <= consumedSequence[slot]) continue;
            const HRESULT hr = mutexes[slot]->AcquireSync(1, 0);
            if (hr != S_OK) continue;
            mutexes[slot]->ReleaseSync(0);
            consumedSequence[slot] = sequence;
        }
    }
};

bool WaitForLateFrames(FcsTestGameIpc* testIpc, FcsIpcV1* captureIpc,
                       SlotDrainer& drainer, LONG64 targetFrames,
                       DWORD timeoutMilliseconds) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    while (GetTickCount64() < deadline) {
        if (testIpc->state == FCS_TEST_GAME_ERROR) {
            wprintf(L"FAIL: synthetic late-overlay error: %ls\n", testIpc->message);
            return false;
        }
        drainer.Drain(captureIpc);
        if (testIpc->lateRehookCompleted &&
            testIpc->latePresentFrames >= targetFrames) {
            return true;
        }
        Sleep(2);
    }
    wprintf(L"FAIL: timed out waiting for late-overlay frames "
            L"(complete=%ld, frames=%lld, installs=%ld)\n",
            testIpc->lateRehookCompleted, testIpc->latePresentFrames,
            testIpc->lateOverlayInstalls);
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

        wchar_t hookPath[MAX_PATH]{};
        if (!BuildSiblingPath(hookPath, MAX_PATH, L"FfxivCleanStreamHook64.dll") ||
            GetFileAttributesW(hookPath) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"FAIL: current production hook DLL is missing beside this test\n");
            break;
        }
        if (!InjectHook(child.dwProcessId, hookPath) ||
            !WaitForCleanFrame(captureIpc, captureFrameEvent)) break;

        StableMetadata metadata{};
        if (!ReadStableMetadata(captureIpc, metadata)) {
            wprintf(L"FAIL: capture metadata never became stable\n");
            break;
        }
        uint32_t redPixelCount = 0;
        if (!VerifyCapturedPixels(captureIpc, metadata, 96, 64,
                                  L"Before late rehook", redPixelCount)) break;

        SlotDrainer drainer;
        if (!drainer.Initialize(metadata)) {
            wprintf(L"FAIL: could not initialize keyed-mutex drainer\n");
            break;
        }

        // Prove the ring is live before changing hook order; otherwise a full
        // ring could make a broken implementation look fail-closed.
        const LONG64 liveFrameId = captureIpc->frameId;
        const ULONGLONG liveDeadline = GetTickCount64() + 3000;
        while (captureIpc->frameId <= liveFrameId && GetTickCount64() < liveDeadline) {
            drainer.Drain(captureIpc);
            WaitForSingleObject(captureFrameEvent, 10);
        }
        if (captureIpc->frameId <= liveFrameId) {
            wprintf(L"FAIL: capture ring was not live before the late rehook\n");
            break;
        }

        InterlockedExchange(&testIpc->lateRehookRequested, 1);
        if (!WaitForLateFrames(testIpc, captureIpc, drainer,
                               kSettleLateFrames, 5000)) break;

        // Snapshot only after two late-overlay frames. A correct helper has
        // synchronously noticed the changed vtable and stopped publishing by
        // this point. Continue draining so the old dynamic-rewrap bug cannot
        // hide behind keyed-mutex backpressure.
        drainer.Drain(captureIpc);
        const LONG64 frameIdAfterSettle = captureIpc->frameId;
        const LONG64 copiesAfterSettle = captureIpc->copiesSubmitted;
        const LONG64 lateFramesAfterSettle = testIpc->latePresentFrames;
        const LONG stateAfterSettle = captureIpc->state;
        if (!WaitForLateFrames(testIpc, captureIpc, drainer,
                               lateFramesAfterSettle + kObservedLateFrames, 5000)) break;

        // Stop only the hidden synthetic child. Do not set the capture stop
        // flag before taking assertions: doing so would mask an unsafe helper.
        InterlockedExchange(&testIpc->stopRequested, 1);
        if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
            wprintf(L"FAIL: synthetic child did not stop after the bounded probe\n");
            break;
        }

        const LONG64 totalLateFrames = testIpc->latePresentFrames;
        const LONG64 totalLateRuns = testIpc->lateOverlayRuns;
        const LONG64 duplicateFrames = testIpc->lateOverlayDuplicateFrames;
        const LONG64 missingFrames = testIpc->lateOverlayMissingFrames;
        const LONG installs = testIpc->lateOverlayInstalls;
        const LONG maxCalls = testIpc->lateOverlayMaxCallsPerPresent;
        const bool exactlyOnce = totalLateFrames >=
                                     kSettleLateFrames + kObservedLateFrames &&
                                 totalLateRuns == totalLateFrames &&
                                 duplicateFrames == 0 && missingFrames == 0 &&
                                 maxCalls == 1;
        const bool overlayStayedInstalled = installs == 1;
        const bool captureStopped = captureIpc->frameId == frameIdAfterSettle &&
                                    captureIpc->copiesSubmitted == copiesAfterSettle;
        const bool hookOrderLossSignaled =
            stateAfterSettle == FCS_STATE_HOOK_ORDER_LOST;
        const bool captureResourcesReleased =
            captureIpc->sharingMode == FCS_SHARING_NONE &&
            captureIpc->width == 0 && captureIpc->height == 0;
        const bool initialOverlayExactlyOnce =
            testIpc->overlayRuns == testIpc->gameFrames;

        wprintf(L"Late rehook: game late-presents=%lld, late-overlay calls=%lld, "
                L"installs=%ld, duplicate-frames=%lld, missing-frames=%lld, max=%ld\n",
                totalLateFrames, totalLateRuns, installs, duplicateFrames,
                missingFrames, maxCalls);
        wprintf(L"Capture after settle: frameId=%lld->%lld, copies=%lld->%lld, "
                L"state=%ld, message=%ls\n",
                frameIdAfterSettle, captureIpc->frameId,
                copiesAfterSettle, captureIpc->copiesSubmitted,
                stateAfterSettle, captureIpc->message);
        wprintf(L"Downstream overlay: calls=%lld, game frames=%lld\n",
                testIpc->overlayRuns, testIpc->gameFrames);

        passed = exactlyOnce && overlayStayedInstalled && captureStopped &&
                 hookOrderLossSignaled && captureResourcesReleased &&
                 initialOverlayExactlyOnce;
        wprintf(passed
            ? L"PASS: late overlay stayed single-call and production capture failed closed\n"
            : L"FAIL: late rehook was amplified, bypassed, or capture kept publishing\n");
    } while (false);

    if (captureIpc) InterlockedExchange(&captureIpc->stopRequested, 1);
    if (testIpc) InterlockedExchange(&testIpc->stopRequested, 1);
    if (child.hProcess && WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) {
        if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
            // This is only the hidden child launched by this test.
            TerminateProcess(child.hProcess, 2);
            WaitForSingleObject(child.hProcess, 1000);
        }
    }
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
