#include "capture_e2e_support.hpp"

// Own the keyed-path end-to-end scenario; reusable mechanics live in the
// focused process, IPC, and GPU support translation units.
#include <stdio.h>
#include <string.h>

using namespace fcs_test;

namespace {

enum class TestPath { HostNtName, HostNtHandle, HostLegacy };

} // namespace

int main(int argc, char** argv) {
    TestPath testPath = TestPath::HostNtName;
    if (argc == 2 && strcmp(argv[1], "--host-nt-handle") == 0) {
        testPath = TestPath::HostNtHandle;
    } else if (argc == 2 && strcmp(argv[1], "--host-legacy") == 0) {
        testPath = TestPath::HostLegacy;
    } else if (argc != 1) {
        printf("Usage: CaptureE2ETest.exe "
               "[--host-nt-handle|--host-legacy]\n");
        return 2;
    }
    const FcsSharingMode expectedHostMode = testPath == TestPath::HostLegacy
        ? FCS_SHARING_HOST_LEGACY_HANDLE
        : FCS_SHARING_HOST_NT_NAME;

    PROCESS_INFORMATION child{};
    HANDLE testMapping = nullptr;
    HANDLE testReadyEvent = nullptr;
    FcsTestGameIpc* testIpc = nullptr;
    HANDLE captureMapping = nullptr;
    HANDLE captureReadyEvent = nullptr;
    HANDLE captureFrameEvent = nullptr;
    FcsIpcV1* captureIpc = nullptr;
    bool passed = false;
    uint32_t redPixelCount = 0;

    do {
        if (!LaunchHiddenSyntheticGame(child)) {
            wprintf(L"FAIL: could not launch hidden synthetic game (%lu)\n",
                    GetLastError());
            break;
        }
        if (!OpenSyntheticTelemetry(child.dwProcessId, testMapping,
                                    testReadyEvent, testIpc)) {
            if (testIpc && testIpc->state == FCS_TEST_GAME_ERROR) {
                wprintf(L"FAIL: synthetic game: %ls\n", testIpc->message);
            } else {
                wprintf(L"FAIL: synthetic game did not become ready\n");
            }
            break;
        }

        const HWND target = reinterpret_cast<HWND>(
            static_cast<uintptr_t>(testIpc->targetHwnd));
        if (!CreateCaptureSession(child.dwProcessId, target, captureMapping,
                                  captureReadyEvent, captureFrameEvent,
                                  captureIpc)) {
            wprintf(L"FAIL: could not create production IPC objects\n");
            break;
        }
        InterlockedOr(&captureIpc->command, FCS_COMMAND_FORCE_HOST_RESOURCES);
        if (testPath == TestPath::HostNtHandle) {
            InterlockedOr(&captureIpc->command,
                          FCS_COMMAND_TEST_REJECT_HOST_NT_NAME);
        } else if (testPath == TestPath::HostLegacy) {
            InterlockedOr(&captureIpc->command, FCS_COMMAND_TEST_REJECT_HOST_NT);
        }

        wchar_t hookPath[MAX_PATH]{};
        if (!BuildSiblingPath(hookPath, MAX_PATH,
                              L"FfxivCleanStreamHook64.dll") ||
            GetFileAttributesW(hookPath) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"FAIL: FfxivCleanStreamHook64.dll is missing beside "
                    L"this test\n");
            break;
        }
        if (!InjectHook(child.dwProcessId, hookPath)) break;
        if (!WaitForCleanFrame(captureIpc, captureFrameEvent)) break;

        StableMetadata metadata{};
        if (!ReadStableMetadata(captureIpc, metadata)) {
            wprintf(L"FAIL: capture metadata never became stable\n");
            break;
        }
        if (metadata.sharingMode != expectedHostMode) {
            wprintf(L"FAIL: E2E reached the wrong host-owned sharing path "
                    L"(mode=%u)\n",
                    static_cast<unsigned int>(metadata.sharingMode));
            break;
        }
        const UINT expectedInitialRequests =
            testPath == TestPath::HostLegacy ? 2u : 1u;
        if (!SawHostNtRequest() ||
            ForcedHostRequestCount() != expectedInitialRequests ||
            (testPath == TestPath::HostLegacy && !SawHostLegacyRequest()) ||
            (testPath != TestPath::HostLegacy && SawHostLegacyRequest())) {
            wprintf(L"FAIL: unexpected host request sequence "
                    L"(count=%u, nt=%u, legacy=%u)\n",
                    ForcedHostRequestCount(), SawHostNtRequest(),
                    SawHostLegacyRequest());
            break;
        }
        if (testPath == TestPath::HostNtHandle &&
            (!FAILED(static_cast<HRESULT>(
                 captureIpc->hostNtNamePathError)) ||
             FAILED(static_cast<HRESULT>(
                 captureIpc->hostNtHandlePathError)))) {
            wprintf(L"FAIL: duplicated NT-handle fallback diagnostics were "
                    L"not recorded\n");
            break;
        }
        if (testPath == TestPath::HostLegacy &&
            (!FAILED(static_cast<HRESULT>(
                 captureIpc->hostNtNamePathError)) ||
             !FAILED(static_cast<HRESULT>(
                 captureIpc->hostNtHandlePathError)) ||
             FAILED(static_cast<HRESULT>(
                 captureIpc->hostLegacyPathError)))) {
            wprintf(L"FAIL: host legacy fallback diagnostics were not "
                    L"recorded\n");
            break;
        }
        if (!VerifyCapturedPixels(captureIpc, metadata, 96, 64, L"Initial",
                                  redPixelCount)) {
            break;
        }

        // The mock hook must have run after the real helper copied the clean
        // frame; otherwise an all-blue capture would be a false positive.
        const ULONGLONG overlayDeadline = GetTickCount64() + 2000;
        while (testIpc->overlayRuns == 0 &&
               GetTickCount64() < overlayDeadline) {
            Sleep(10);
        }
        if (testIpc->overlayRuns == 0) {
            wprintf(L"FAIL: mock red overlay never executed\n");
            break;
        }

        const LONG64 initialGeneration = metadata.generation;
        const LONG64 initialFrameId = captureIpc->frameId;
        if (testPath == TestPath::HostNtHandle) {
            InterlockedOr(&captureIpc->command,
                          FCS_COMMAND_TEST_REJECT_HOST_NT_NAME);
        }
        if (!RequestResizeAndWaitForNewFrame(
                testIpc, captureIpc, captureFrameEvent, initialGeneration,
                initialFrameId)) {
            break;
        }

        StableMetadata resizedMetadata{};
        if (!ReadStableMetadata(captureIpc, resizedMetadata)) {
            wprintf(L"FAIL: post-resize capture metadata never became stable\n");
            break;
        }
        if (resizedMetadata.generation <= initialGeneration) {
            wprintf(L"FAIL: resize did not publish a new resource generation\n");
            break;
        }
        if (resizedMetadata.sharingMode != expectedHostMode ||
            resizedMetadata.resourceRequestId == metadata.resourceRequestId) {
            wprintf(L"FAIL: resize did not replace the host-owned frame ring "
                    L"(mode=%u, request=%lld->%lld)\n",
                    static_cast<unsigned int>(resizedMetadata.sharingMode),
                    static_cast<long long>(metadata.resourceRequestId),
                    static_cast<long long>(
                        resizedMetadata.resourceRequestId));
            break;
        }
        if (ForcedHostRequestCount() != expectedInitialRequests + 1) {
            wprintf(L"FAIL: resize used an unexpected number of host requests "
                    L"(%u)\n",
                    ForcedHostRequestCount());
            break;
        }
        if (!VerifyCapturedPixels(captureIpc, resizedMetadata, 128, 80,
                                  L"Post-resize", redPixelCount)) {
            break;
        }

        const wchar_t* pathName = testPath == TestPath::HostNtName
            ? L"host NT name"
            : (testPath == TestPath::HostNtHandle
                   ? L"host duplicated NT handle"
                   : L"host legacy handle");
        wprintf(L"PASS: %ls capture survived ResizeBuffers; native HUD "
                L"retained and red overlay excluded\n",
                pathName);
        wprintf(L"Synthetic frames=%lld, overlay runs=%lld, resize count=%ld, "
                L"clean copies=%lld, busy drops=%lld\n",
                testIpc->gameFrames, testIpc->overlayRuns,
                testIpc->resizeCompleted, captureIpc->copiesSubmitted,
                captureIpc->busyDropped);
        passed = true;
    } while (false);

    if (captureIpc) InterlockedExchange(&captureIpc->stopRequested, 1);
    if (testIpc) InterlockedExchange(&testIpc->stopRequested, 1);
    if (child.hProcess &&
        WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) {
        // This is only the hidden child launched by this bounded test.
        TerminateProcess(child.hProcess, 2);
        WaitForSingleObject(child.hProcess, 1000);
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
