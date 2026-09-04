#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stddef.h>
#include <stdint.h>

#include "../common/fcs_ipc.hpp"
#include "e2e_test_protocol.h"

namespace fcs_test {

template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct StableMetadata {
    LONG64 generation;
    wchar_t names[FCS_SLOT_COUNT][96];
    uint64_t handles[FCS_SLOT_COUNT];
    FcsSharingMode sharingMode;
    uint64_t adapterLuid;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LONG64 resourceRequestId;
    FcsSharingMode requestedHostMode;
};

bool BuildSiblingPath(wchar_t* output, size_t outputChars, const wchar_t* fileName);
bool InjectHook(DWORD pid, const wchar_t* dllPath);
IDXGIAdapter1* FindAdapter(uint64_t packedLuid);
bool ReadStableMetadata(FcsIpcV1* ipc, StableMetadata& output);
void DestroyForcedHostRing();
UINT ForcedHostRequestCount();
bool SawHostNtRequest();
bool SawHostLegacyRequest();
HRESULT OpenSharedTexture(ID3D11Device* device, ID3D11Device1* device1,
                          const StableMetadata& metadata, UINT slot,
                          ID3D11Texture2D** texture);
bool LaunchHiddenSyntheticGame(PROCESS_INFORMATION& processInfo);
bool OpenSyntheticTelemetry(DWORD pid, HANDLE& mapping, HANDLE& readyEvent,
                            FcsTestGameIpc*& ipc);
bool CreateCaptureSession(DWORD pid, HWND targetHwnd, HANDLE& mapping,
                          HANDLE& readyEvent, HANDLE& frameEvent, FcsIpcV1*& ipc);
bool WaitForCleanFrame(FcsIpcV1* ipc, HANDLE frameEvent);
bool RequestResizeAndWaitForNewFrame(FcsTestGameIpc* testIpc, FcsIpcV1* captureIpc,
                                     HANDLE frameEvent, LONG64 oldGeneration,
                                     LONG64 oldFrameId);
bool VerifyCapturedPixels(FcsIpcV1* ipc, const StableMetadata& metadata,
                          UINT expectedWidth, UINT expectedHeight,
                          const wchar_t* phase, uint32_t& redPixelCount);

} // namespace fcs_test
