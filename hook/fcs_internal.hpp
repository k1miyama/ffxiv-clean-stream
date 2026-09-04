#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "../common/fcs_formats.hpp"
#include "../common/fcs_ipc.hpp"

namespace fcs::hook {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);

struct ProxyRecord {
    IDXGISwapChain* swap;
    void** proxyVtable;
    PresentFn nextPresent;
    Present1Fn nextPresent1;
    size_t vtableSlots;
    volatile LONG valid;
};

struct PendingFrame {
    bool copied;
    LONG slot;
    LONG64 sequence;
    LONG64 qpc;
    LONG64 generation;
};

enum PlainSlotState : LONG {
    PLAIN_SLOT_FREE = 0,
    PLAIN_SLOT_COPY_PENDING = 1,
    PLAIN_SLOT_PUBLISHED = 2,
};

inline constexpr LONG kMaxProxyRecords = 64;
inline constexpr size_t kSwapChainSlots = 18;
inline constexpr size_t kSwapChain1Slots = 29;
inline constexpr size_t kSwapChain2Slots = 36;
inline constexpr size_t kSwapChain3Slots = 40;
inline constexpr size_t kSwapChain4Slots = 41;

struct HostResourceResponse {
    LONG64 generation;
    LONG64 requestId;
    uint32_t controllerPid;
    FcsSharingMode sharingMode;
    HRESULT error;
    FcsResourceStage failureStage;
    wchar_t names[FCS_SLOT_COUNT][96];
    uint64_t handles[FCS_SLOT_COUNT];
};

struct IpcRuntimeState {
    FcsIpcV1* ipc;
    HANDLE mapping;
    HANDLE readyEvent;
    HANDLE frameEvent;
};

struct SwapChainRuntimeState {
    PresentFn bootstrapOriginal;
    Present1Fn bootstrapOriginal1;
    void* presentTarget;
    void* present1Target;
    ProxyRecord records[kMaxProxyRecords];
    volatile LONG recordCount;
    volatile LONG latestRecord;
    IDXGISwapChain* candidate;
    LONG candidatePresents;
    SRWLOCK wrapLock;
    volatile LONG hookOrderLost;
};

struct CaptureRuntimeState {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* sharedTextures[FCS_SLOT_COUNT];
    IDXGIKeyedMutex* keyedMutexes[FCS_SLOT_COUNT];
    HANDLE sharedNtHandles[FCS_SLOT_COUNT];
    ID3D11Query* plainProducerQueries[FCS_SLOT_COUNT];
    PlainSlotState plainSlotStates[FCS_SLOT_COUNT];
    LONG64 plainSlotSequences[FCS_SLOT_COUNT];
    LONG64 plainSlotRingIds[FCS_SLOT_COUNT];
    LONG64 plainSlotQpc[FCS_SLOT_COUNT];
    UINT width;
    UINT height;
    DXGI_FORMAT sourceFormat;
    DXGI_FORMAT transportFormat;
    UINT sourceSamples;
    IDXGISwapChain* resourceSwap;
    LONG nextSlot;
    LONG64 sequence;
    LONG64 lastCaptureQpc;
    LONG64 resourceRetryAfterQpc;
    LONG64 nextExternalRecreateQpc;
    LONG64 presentCounter;
    LONG64 rateSkippedCounter;
    volatile LONG captureGate;
    volatile LONG sourceChanged;
    LARGE_INTEGER qpcFrequency;
    FcsSharingMode sharingMode;
    FcsSharingMode requestedHostMode;
    LONG64 activeHostRequestId;
    LONG64 lastHostAttemptGeneration;
    uint64_t adapterLuid;
    bool hostFallbackTerminal;
    bool preferHostLegacy;
    bool preferPlainLegacy;
    HRESULT ntCreateError;
    HRESULT legacyCreateError;
    HRESULT plainCreateError;
    FcsResourceStage ntCreateStage;
    FcsResourceStage legacyCreateStage;
    FcsResourceStage plainCreateStage;
    HRESULT hostNtNamePathError;
    HRESULT hostNtHandlePathError;
    HRESULT hostLegacyPathError;
    FcsResourceStage hostNtNamePathStage;
    FcsResourceStage hostNtHandlePathStage;
    FcsResourceStage hostLegacyPathStage;
};

struct HookProcessState {
    HMODULE module;
};

// These are actual zero-initialized subgroup objects, not reference aliases.
// Each implementation module names only the state groups it participates in,
// while fcs_state.cpp remains their single process-lifetime owner.
extern HookProcessState g_processState;
extern IpcRuntimeState g_ipcState;
extern SwapChainRuntimeState g_swapChainState;
extern CaptureRuntimeState g_captureState;

void InitializeRuntimeState(HMODULE module);

template <typename T>
inline void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool TryEnterCaptureGate();
void LeaveCaptureGate();
void SetMessage(const wchar_t* text);
void SetError(LONG code, const wchar_t* text);
void BeginMetadataWrite();
void EndMetadataWrite();
void InvalidatePublishedResources();
void ReleaseSharedRing();
void ReleaseCaptureResources();
bool EnsureCaptureResources(IDXGISwapChain* swap,
                            const D3D11_TEXTURE2D_DESC& desc);

bool ActivatePlainLegacyFallback();
void PublishHostResourceRequest(FcsSharingMode requestedMode);
bool TryOpenHostOwnedRing();

HRESULT CreatePlainLegacySharedRing(const D3D11_TEXTURE2D_DESC& baseDesc,
                                    uint64_t (&sharedHandles)[FCS_SLOT_COUNT],
                                    FcsResourceStage& failedStage);
HRESULT CreateNtSharedRing(const D3D11_TEXTURE2D_DESC& sharedDesc,
                           wchar_t (&resourceNames)[FCS_SLOT_COUNT][96],
                           FcsResourceStage& failedStage);
HRESULT CreateLegacySharedRing(const D3D11_TEXTURE2D_DESC& baseDesc,
                               uint64_t (&sharedHandles)[FCS_SLOT_COUNT],
                               FcsResourceStage& failedStage);
HRESULT OpenHostNtRingByName(ID3D11Device1* device1,
                             const HostResourceResponse& response,
                             FcsResourceStage& failedStage);
HRESULT OpenHostNtRingByHandle(ID3D11Device1* device1,
                               const HostResourceResponse& response,
                               FcsResourceStage& failedStage);
HRESULT OpenHostLegacyRing(const HostResourceResponse& response,
                           FcsResourceStage& failedStage);

PendingFrame TryCopyCleanFrame(IDXGISwapChain* swap);
void PublishFrame(const PendingFrame& frame);
void HandleDeviceLoss(HRESULT hr, PendingFrame& pending);
void ReleaseResourcesAfterHookOrderLoss();

HRESULT STDMETHODCALLTYPE BootstrapPresent(IDXGISwapChain* swap,
                                            UINT syncInterval, UINT flags);
HRESULT STDMETHODCALLTYPE BootstrapPresent1(
    IDXGISwapChain1* swap, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters);

DWORD WINAPI HookWorker(void* module);

} // namespace fcs::hook
