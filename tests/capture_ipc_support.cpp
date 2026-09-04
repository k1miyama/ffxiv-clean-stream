#include "capture_e2e_support_internal.hpp"

#include <stdio.h>

namespace fcs_test {

bool ReadStableMetadata(FcsIpcV1* ipc, StableMetadata& output) {
    if (!ipc || ipc->magic != FCS_MAGIC || ipc->version != FCS_VERSION ||
        ipc->bytes != sizeof(FcsIpcV1)) {
        return false;
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        const LONG64 before = ipc->generation;
        MemoryBarrier();
        if (before & 1) {
            Sleep(1);
            continue;
        }
        output.generation = before;
        output.adapterLuid = ipc->adapterLuid;
        output.width = ipc->width;
        output.height = ipc->height;
        output.format = static_cast<DXGI_FORMAT>(ipc->format);
        output.sharingMode = static_cast<FcsSharingMode>(ipc->sharingMode);
        output.resourceRequestId = ipc->resourceRequestId;
        output.requestedHostMode =
            static_cast<FcsSharingMode>(ipc->hostRequestedSharingMode);
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            lstrcpynW(output.names[slot], ipc->sharedNames[slot], 96);
            output.handles[slot] = ipc->sharedHandles[slot];
        }
        MemoryBarrier();
        const LONG64 after = ipc->generation;
        if (before == after && !(after & 1) && output.width && output.height &&
            output.format != DXGI_FORMAT_UNKNOWN) {
            bool complete = true;
            for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
                if ((output.sharingMode == FCS_SHARING_NT_NAME ||
                     output.sharingMode == FCS_SHARING_HOST_NT_NAME) &&
                    !output.names[slot][0]) {
                    complete = false;
                }
                if ((output.sharingMode == FCS_SHARING_LEGACY_HANDLE ||
                     output.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE ||
                     output.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE) &&
                    !output.handles[slot]) {
                    complete = false;
                }
            }
            if (output.sharingMode != FCS_SHARING_NT_NAME &&
                output.sharingMode != FCS_SHARING_LEGACY_HANDLE &&
                output.sharingMode != FCS_SHARING_HOST_NT_NAME &&
                output.sharingMode != FCS_SHARING_HOST_LEGACY_HANDLE &&
                output.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE &&
                output.sharingMode != FCS_SHARING_HOST_REQUEST) {
                complete = false;
            }
            if ((output.sharingMode == FCS_SHARING_HOST_REQUEST ||
                 output.sharingMode == FCS_SHARING_HOST_NT_NAME ||
                 output.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE ||
                 output.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE) &&
                !output.resourceRequestId) {
                complete = false;
            }
            if (output.sharingMode == FCS_SHARING_HOST_REQUEST &&
                output.requestedHostMode != FCS_SHARING_HOST_NT_NAME &&
                output.requestedHostMode != FCS_SHARING_HOST_LEGACY_HANDLE) {
                complete = false;
            }
            if (complete) return true;
        }
        Sleep(1);
    }
    return false;
}

bool OpenSyntheticTelemetry(DWORD pid, HANDLE& mapping, HANDLE& readyEvent,
                            FcsTestGameIpc*& ipc) {
    wchar_t name[96]{};
    FcsTestGameMappingName(name, pid);
    for (int attempt = 0; attempt < 100; ++attempt) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (mapping) break;
        Sleep(50);
    }
    if (!mapping) return false;
    ipc = static_cast<FcsTestGameIpc*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FcsTestGameIpc)));
    if (!ipc || ipc->magic != FCS_TEST_MAGIC) return false;

    FcsTestGameReadyEventName(name, pid);
    for (int attempt = 0; attempt < 100; ++attempt) {
        readyEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
        if (readyEvent) break;
        Sleep(25);
    }
    if (!readyEvent ||
        WaitForSingleObject(readyEvent, 5000) != WAIT_OBJECT_0) {
        return false;
    }
    return ipc->state == FCS_TEST_GAME_READY && ipc->targetHwnd != 0;
}

bool CreateCaptureSession(DWORD pid, HWND targetHwnd, HANDLE& mapping,
                          HANDLE& readyEvent, HANDLE& frameEvent,
                          FcsIpcV1*& ipc) {
    wchar_t name[96]{};
    FcsMappingName(name, pid);
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, sizeof(FcsIpcV1), name);
    if (!mapping) return false;
    ipc = static_cast<FcsIpcV1*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FcsIpcV1)));
    if (!ipc) return false;
    ZeroMemory(ipc, sizeof(*ipc));
    ipc->magic = FCS_MAGIC;
    ipc->version = FCS_VERSION;
    ipc->bytes = sizeof(*ipc);
    ipc->controllerPid = GetCurrentProcessId();
    ipc->targetHwnd = reinterpret_cast<uint64_t>(targetHwnd);
    ipc->captureFps = 60;
    ipc->latestSlot = -1;

    FcsReadyEventName(name, pid);
    readyEvent = CreateEventW(nullptr, FALSE, FALSE, name);
    FcsFrameEventName(name, pid);
    frameEvent = CreateEventW(nullptr, FALSE, FALSE, name);
    return readyEvent && frameEvent;
}

bool WaitForCleanFrame(FcsIpcV1* ipc, HANDLE frameEvent) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (!detail::ServiceForcedHostResources(ipc)) {
            wprintf(L"FAIL: test host could not create the forced host-owned "
                    L"frame ring\n");
            return false;
        }
        if (ipc->state == FCS_STATE_ERROR) {
            wprintf(L"FAIL: injected helper error %ld: %ls\n", ipc->lastError,
                    ipc->message);
            return false;
        }
        if (ipc->state == FCS_STATE_STREAMING && ipc->frameId > 0) return true;
        WaitForSingleObject(frameEvent, 100);
    }
    wprintf(L"FAIL: timed out waiting for the injected helper "
            L"(state=%ld, message=%ls)\n",
            ipc->state, ipc->message);
    return false;
}

bool RequestResizeAndWaitForNewFrame(FcsTestGameIpc* testIpc,
                                     FcsIpcV1* captureIpc,
                                     HANDLE frameEvent,
                                     LONG64 oldGeneration,
                                     LONG64 oldFrameId) {
    const LONG64 oldOverlayRuns = testIpc->overlayRuns;
    InterlockedExchange(&testIpc->resizeRequested, 1);
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (!detail::ServiceForcedHostResources(captureIpc)) {
            wprintf(L"FAIL: test host could not recreate the host-owned "
                    L"frame ring\n");
            return false;
        }
        if (testIpc->state == FCS_TEST_GAME_ERROR) {
            wprintf(L"FAIL: synthetic resize error: %ls\n", testIpc->message);
            return false;
        }
        if (captureIpc->state == FCS_STATE_ERROR) {
            wprintf(L"FAIL: helper error after resize %ld: %ls\n",
                    captureIpc->lastError, captureIpc->message);
            return false;
        }
        if (testIpc->resizeCompleted == 1 && testIpc->resizedWidth == 128 &&
            testIpc->resizedHeight == 80 &&
            captureIpc->generation > oldGeneration &&
            !(captureIpc->generation & 1) &&
            captureIpc->frameId > oldFrameId && captureIpc->width == 128 &&
            captureIpc->height == 80 &&
            testIpc->overlayRuns > oldOverlayRuns) {
            return true;
        }
        WaitForSingleObject(frameEvent, 100);
    }
    wprintf(L"FAIL: no clean post-resize frame "
            L"(resize=%ld, game-size=%ldx%ld, capture-size=%ux%u, "
            L"generation=%lld->%lld, frame=%lld->%lld, state=%ld, "
            L"overlay=%lld->%lld)\n",
            testIpc->resizeCompleted, testIpc->resizedWidth,
            testIpc->resizedHeight, captureIpc->width, captureIpc->height,
            oldGeneration, captureIpc->generation, oldFrameId,
            captureIpc->frameId, captureIpc->state, oldOverlayRuns,
            testIpc->overlayRuns);
    return false;
}

} // namespace fcs_test
