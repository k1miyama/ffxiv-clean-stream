// Reuse the hidden target launcher, production-DLL injector, and IPC helpers
// from the end-to-end correctness fixture without duplicating test machinery.
#include "capture_e2e_support.hpp"

#include <stdio.h>

using namespace fcs_test;

namespace {

constexpr DWORD kBaselineMilliseconds = 3000;
constexpr DWORD kCaptureMilliseconds = 3000;
constexpr DWORD kWarmupMilliseconds = 500;

struct FrameDrainer {
    ID3D11Device* device = nullptr;
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* shared[FCS_SLOT_COUNT]{};
    IDXGIKeyedMutex* mutexes[FCS_SLOT_COUNT]{};
    ID3D11Texture2D* sink = nullptr;
    LONG64 consumedSequence[FCS_SLOT_COUNT]{};

    ~FrameDrainer() {
        ReleaseCom(sink);
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

        D3D11_TEXTURE2D_DESC sinkDesc{};
        shared[0]->GetDesc(&sinkDesc);
        sinkDesc.Usage = D3D11_USAGE_DEFAULT;
        sinkDesc.BindFlags = 0;
        sinkDesc.CPUAccessFlags = 0;
        sinkDesc.MiscFlags = 0;
        return SUCCEEDED(device->CreateTexture2D(&sinkDesc, nullptr, &sink));
    }

    UINT DrainAvailable(FcsIpcV1* ipc) {
        UINT drained = 0;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = ipc->slotSequence[slot];
            if (sequence <= consumedSequence[slot]) continue;
            const HRESULT hr = mutexes[slot]->AcquireSync(1, 0);
            if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) continue;
            if (hr != S_OK) continue;
            context->CopyResource(sink, shared[slot]);
            mutexes[slot]->ReleaseSync(0);
            consumedSequence[slot] = sequence;
            ++drained;
        }
        // The consumer, not the injected game-process helper, flushes its own
        // tiny preview copy workload. This keeps the ring progressing while
        // preserving the production helper's no-Flush hot path.
        if (drained) context->Flush();
        return drained;
    }
};

double SecondsBetween(const LARGE_INTEGER& begin, const LARGE_INTEGER& end,
                      const LARGE_INTEGER& frequency) {
    return static_cast<double>(end.QuadPart - begin.QuadPart) /
           static_cast<double>(frequency.QuadPart);
}

double MeasureFrames(FcsTestGameIpc* testIpc, DWORD durationMilliseconds,
                     LONG64& frameDelta) {
    LARGE_INTEGER frequency{}, begin{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    const LONG64 firstFrame = testIpc->gameFrames;
    do {
        Sleep(1);
        QueryPerformanceCounter(&now);
    } while (SecondsBetween(begin, now, frequency) * 1000.0 < durationMilliseconds);
    frameDelta = testIpc->gameFrames - firstFrame;
    return SecondsBetween(begin, now, frequency);
}

double MeasureCapturedFrames(FcsTestGameIpc* testIpc, FcsIpcV1* captureIpc,
                             HANDLE frameEvent, FrameDrainer& drainer,
                             DWORD durationMilliseconds, LONG64& frameDelta,
                             UINT64& drainedFrames) {
    LARGE_INTEGER frequency{}, begin{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    const LONG64 firstFrame = testIpc->gameFrames;
    drainedFrames = 0;
    do {
        WaitForSingleObject(frameEvent, 2);
        drainedFrames += drainer.DrainAvailable(captureIpc);
        QueryPerformanceCounter(&now);
    } while (SecondsBetween(begin, now, frequency) * 1000.0 < durationMilliseconds);
    drainedFrames += drainer.DrainAvailable(captureIpc);
    frameDelta = testIpc->gameFrames - firstFrame;
    return SecondsBetween(begin, now, frequency);
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

        Sleep(kWarmupMilliseconds);
        LONG64 baselineFrames = 0;
        const double baselineSeconds = MeasureFrames(
            testIpc, kBaselineMilliseconds, baselineFrames);

        const HWND target = reinterpret_cast<HWND>(
            static_cast<uintptr_t>(testIpc->targetHwnd));
        if (!CreateCaptureSession(child.dwProcessId, target, captureMapping,
                                  captureReadyEvent, captureFrameEvent, captureIpc)) {
            wprintf(L"FAIL: could not create production IPC objects\n");
            break;
        }
        InterlockedExchange(&captureIpc->captureFps, 30);

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
        FrameDrainer drainer;
        if (!drainer.Initialize(metadata)) {
            wprintf(L"FAIL: could not initialize the D3D11 frame consumer\n");
            break;
        }

        const ULONGLONG warmDeadline = GetTickCount64() + kWarmupMilliseconds;
        UINT64 warmDrains = 0;
        while (GetTickCount64() < warmDeadline) {
            WaitForSingleObject(captureFrameEvent, 2);
            warmDrains += drainer.DrainAvailable(captureIpc);
        }
        if (!warmDrains) {
            wprintf(L"FAIL: consumer did not drain a warm-up frame\n");
            break;
        }

        const LONG64 copiesBefore = captureIpc->copiesSubmitted;
        const LONG64 busyBefore = captureIpc->busyDropped;
        const LONG64 skippedBefore = captureIpc->rateSkipped;
        InterlockedExchange64(&captureIpc->hookCpuTicksMax, 0);

        LONG64 capturedFrames = 0;
        UINT64 drainedFrames = 0;
        const double capturedSeconds = MeasureCapturedFrames(
            testIpc, captureIpc, captureFrameEvent, drainer,
            kCaptureMilliseconds, capturedFrames, drainedFrames);

        const LONG64 copies = captureIpc->copiesSubmitted - copiesBefore;
        const LONG64 busyDrops = captureIpc->busyDropped - busyBefore;
        const LONG64 rateSkips = captureIpc->rateSkipped - skippedBefore;
        const double baselineFps = static_cast<double>(baselineFrames) / baselineSeconds;
        const double capturedFps = static_cast<double>(capturedFrames) / capturedSeconds;
        const double deltaFps = capturedFps - baselineFps;
        const double deltaPercent = baselineFps > 0.0
            ? (deltaFps / baselineFps) * 100.0 : 0.0;
        const double hookMaxMicroseconds = captureIpc->qpcFrequency > 0
            ? static_cast<double>(captureIpc->hookCpuTicksMax) * 1000000.0 /
              static_cast<double>(captureIpc->qpcFrequency)
            : -1.0;

        wprintf(L"Method: one hidden 96x64 D3D11 target with downstream mock overlay; "
                L"3.0 s baseline, then current production DLL at 30 FPS for 3.0 s.\n");
        wprintf(L"Baseline: %lld frames / %.3f s = %.2f FPS\n",
                baselineFrames, baselineSeconds, baselineFps);
        wprintf(L"Capture:  %lld frames / %.3f s = %.2f FPS\n",
                capturedFrames, capturedSeconds, capturedFps);
        wprintf(L"Delta:    %+.2f FPS (%+.2f%%)\n", deltaFps, deltaPercent);
        wprintf(L"Capture health: submitted=%lld drained=%llu busy-dropped=%lld "
                L"rate-skipped=%lld; hook max CPU submit=%.2f us\n",
                copies, static_cast<unsigned long long>(drainedFrames), busyDrops,
                rateSkips, hookMaxMicroseconds);
        wprintf(L"Overlay health: baseline+capture overlay runs=%lld\n",
                testIpc->overlayRuns);

        // The hidden legacy swap chain is compositor-throttled on some GPUs.
        // A 30-FPS request can therefore land on every third ~60-Hz Present;
        // health is established by sustained publication and a fully drained,
        // non-stalling ring rather than by requiring exactly 90 copies.
        passed = baselineFrames > 0 && capturedFrames > 0 && copies >= 40 &&
                 drainedFrames + 2 >= static_cast<UINT64>(copies) && busyDrops == 0 &&
                 testIpc->overlayRuns >= baselineFrames + capturedFrames;
        wprintf(passed
            ? L"PASS: bounded capture throughput test completed with a healthy drained ring\n"
            : L"FAIL: capture ring or synthetic overlay was not healthy during measurement\n");
    } while (false);

    if (captureIpc) InterlockedExchange(&captureIpc->stopRequested, 1);
    if (testIpc) InterlockedExchange(&testIpc->stopRequested, 1);
    if (child.hProcess) {
        if (WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) {
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
