#include "fcs_internal.hpp"

namespace fcs::hook {
namespace {

void SetHostFallbackError() {
    wchar_t text[256]{};
    swprintf_s(text, sizeof(text) / sizeof(text[0]),
               L"All GPU-sharing paths failed. Game NT 0x%08lX; "
               L"game legacy 0x%08lX; host NT name 0x%08lX; "
               L"host NT handle 0x%08lX; host legacy 0x%08lX; "
               L"plain legacy 0x%08lX.",
               static_cast<unsigned long>(static_cast<uint32_t>(g_captureState.ntCreateError)),
               static_cast<unsigned long>(static_cast<uint32_t>(g_captureState.legacyCreateError)),
               static_cast<unsigned long>(static_cast<uint32_t>(g_captureState.hostNtNamePathError)),
               static_cast<unsigned long>(static_cast<uint32_t>(g_captureState.hostNtHandlePathError)),
               static_cast<unsigned long>(static_cast<uint32_t>(g_captureState.hostLegacyPathError)),
               static_cast<unsigned long>(static_cast<uint32_t>(g_captureState.plainCreateError)));
    g_captureState.hostFallbackTerminal = true;
    SetError(static_cast<LONG>(g_captureState.plainCreateError), text);
}

} // namespace

bool ActivatePlainLegacyFallback() {
    if (!g_captureState.device || !g_captureState.context || !g_ipcState.ipc || !g_captureState.width || !g_captureState.height ||
        g_captureState.transportFormat == DXGI_FORMAT_UNKNOWN) {
        g_captureState.plainCreateError = E_UNEXPECTED;
        g_captureState.plainCreateStage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
        SetHostFallbackError();
        return false;
    }

    D3D11_TEXTURE2D_DESC sharedDesc{};
    sharedDesc.Width = g_captureState.width;
    sharedDesc.Height = g_captureState.height;
    sharedDesc.MipLevels = 1;
    sharedDesc.ArraySize = 1;
    sharedDesc.Format = g_captureState.transportFormat;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    uint64_t handles[FCS_SLOT_COUNT]{};
    g_captureState.plainCreateStage = FCS_RESOURCE_STAGE_NONE;
    g_captureState.plainCreateError = CreatePlainLegacySharedRing(
        sharedDesc, handles, g_captureState.plainCreateStage);
    if (FAILED(g_captureState.plainCreateError)) {
        ReleaseSharedRing();
        BeginMetadataWrite();
        g_ipcState.ipc->producerPlainStage = g_captureState.plainCreateStage;
        g_ipcState.ipc->producerPlainError = static_cast<LONG>(g_captureState.plainCreateError);
        g_ipcState.ipc->hostNtNamePathError = static_cast<LONG>(g_captureState.hostNtNamePathError);
        g_ipcState.ipc->hostNtHandlePathError = static_cast<LONG>(g_captureState.hostNtHandlePathError);
        g_ipcState.ipc->hostLegacyPathError = static_cast<LONG>(g_captureState.hostLegacyPathError);
        g_ipcState.ipc->hostNtNamePathStage = g_captureState.hostNtNamePathStage;
        g_ipcState.ipc->hostNtHandlePathStage = g_captureState.hostNtHandlePathStage;
        g_ipcState.ipc->hostLegacyPathStage = g_captureState.hostLegacyPathStage;
        EndMetadataWrite();
        SetHostFallbackError();
        return false;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LONG64 requestId = now.QuadPart;
    if (requestId <= g_ipcState.ipc->resourceRequestId) {
        requestId = g_ipcState.ipc->resourceRequestId + 1;
    }
    g_captureState.sharingMode = FCS_SHARING_LEGACY_PLAIN_HANDLE;
    g_captureState.requestedHostMode = FCS_SHARING_NONE;
    g_captureState.activeHostRequestId = 0;
    g_captureState.lastHostAttemptGeneration = -1;
    g_captureState.hostFallbackTerminal = false;
    g_captureState.preferPlainLegacy = true;
    g_captureState.nextSlot = 0;

    BeginMetadataWrite();
    g_ipcState.ipc->producerPid = GetCurrentProcessId();
    g_ipcState.ipc->adapterLuid = g_captureState.adapterLuid;
    g_ipcState.ipc->width = g_captureState.width;
    g_ipcState.ipc->height = g_captureState.height;
    g_ipcState.ipc->format = static_cast<uint32_t>(g_captureState.transportFormat);
    g_ipcState.ipc->sampleCount = g_captureState.sourceSamples;
    g_ipcState.ipc->sharingMode = FCS_SHARING_LEGACY_PLAIN_HANDLE;
    g_ipcState.ipc->resourceRequestId = requestId;
    g_ipcState.ipc->hostRequestedSharingMode = FCS_SHARING_NONE;
    g_ipcState.ipc->producerNtStage = g_captureState.ntCreateStage;
    g_ipcState.ipc->producerLegacyStage = g_captureState.legacyCreateStage;
    g_ipcState.ipc->producerNtError = static_cast<LONG>(g_captureState.ntCreateError);
    g_ipcState.ipc->producerLegacyError = static_cast<LONG>(g_captureState.legacyCreateError);
    g_ipcState.ipc->producerPlainStage = FCS_RESOURCE_STAGE_NONE;
    g_ipcState.ipc->producerPlainError = S_OK;
    g_ipcState.ipc->hostNtNamePathError = static_cast<LONG>(g_captureState.hostNtNamePathError);
    g_ipcState.ipc->hostNtHandlePathError = static_cast<LONG>(g_captureState.hostNtHandlePathError);
    g_ipcState.ipc->hostLegacyPathError = static_cast<LONG>(g_captureState.hostLegacyPathError);
    g_ipcState.ipc->hostNtNamePathStage = g_captureState.hostNtNamePathStage;
    g_ipcState.ipc->hostNtHandlePathStage = g_captureState.hostNtHandlePathStage;
    g_ipcState.ipc->hostLegacyPathStage = g_captureState.hostLegacyPathStage;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        g_ipcState.ipc->sharedNames[slot][0] = L'\0';
        g_ipcState.ipc->sharedHandles[slot] = handles[slot];
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[slot], 0);
    }
    InterlockedExchange(&g_ipcState.ipc->latestSlot, -1);
    EndMetadataWrite();

    InterlockedExchange(&g_ipcState.ipc->lastError, S_OK);
    SetMessage(L"Clean frames are reaching the preview using the nonblocking compatibility ring.");
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_STREAMING);
    return true;
}

namespace {

bool ReadHostResourceResponse(HostResourceResponse& response) {
    if (!g_ipcState.ipc || g_ipcState.ipc->magic != FCS_MAGIC ||
        g_ipcState.ipc->version != FCS_VERSION || g_ipcState.ipc->bytes != sizeof(FcsIpcV1)) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG64 before = g_ipcState.ipc->hostGeneration;
        MemoryBarrier();
        if (before & 1) continue;
        response.requestId = g_ipcState.ipc->hostRequestId;
        response.controllerPid = g_ipcState.ipc->hostControllerPid;
        response.sharingMode =
            static_cast<FcsSharingMode>(g_ipcState.ipc->hostResponseSharingMode);
        response.error = static_cast<HRESULT>(g_ipcState.ipc->hostResourceError);
        response.failureStage = static_cast<FcsResourceStage>(g_ipcState.ipc->hostFailureStage);
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            lstrcpynW(response.names[slot], g_ipcState.ipc->hostSharedNames[slot], 96);
            response.handles[slot] = g_ipcState.ipc->hostSharedHandles[slot];
        }
        MemoryBarrier();
        const LONG64 after = g_ipcState.ipc->hostGeneration;
        if (before == after && !(after & 1)) {
            response.generation = after;
            return true;
        }
    }
    return false;
}

} // namespace

void PublishHostResourceRequest(FcsSharingMode requestedMode) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LONG64 requestId = now.QuadPart;
    if (requestId <= g_ipcState.ipc->resourceRequestId) requestId = g_ipcState.ipc->resourceRequestId + 1;

    g_captureState.activeHostRequestId = requestId;
    g_captureState.requestedHostMode = requestedMode;
    g_captureState.lastHostAttemptGeneration = -1;
    g_captureState.hostFallbackTerminal = false;
    g_captureState.sharingMode = FCS_SHARING_HOST_REQUEST;

    BeginMetadataWrite();
    g_ipcState.ipc->producerPid = GetCurrentProcessId();
    g_ipcState.ipc->adapterLuid = g_captureState.adapterLuid;
    g_ipcState.ipc->width = g_captureState.width;
    g_ipcState.ipc->height = g_captureState.height;
    g_ipcState.ipc->format = static_cast<uint32_t>(g_captureState.transportFormat);
    g_ipcState.ipc->sampleCount = g_captureState.sourceSamples;
    g_ipcState.ipc->sharingMode = FCS_SHARING_HOST_REQUEST;
    g_ipcState.ipc->producerNtError = static_cast<LONG>(g_captureState.ntCreateError);
    g_ipcState.ipc->producerLegacyError = static_cast<LONG>(g_captureState.legacyCreateError);
    g_ipcState.ipc->producerPlainError = static_cast<LONG>(g_captureState.plainCreateError);
    g_ipcState.ipc->producerNtStage = g_captureState.ntCreateStage;
    g_ipcState.ipc->producerLegacyStage = g_captureState.legacyCreateStage;
    g_ipcState.ipc->producerPlainStage = g_captureState.plainCreateStage;
    g_ipcState.ipc->hostNtNamePathError = static_cast<LONG>(g_captureState.hostNtNamePathError);
    g_ipcState.ipc->hostNtHandlePathError = static_cast<LONG>(g_captureState.hostNtHandlePathError);
    g_ipcState.ipc->hostLegacyPathError = static_cast<LONG>(g_captureState.hostLegacyPathError);
    g_ipcState.ipc->hostNtNamePathStage = g_captureState.hostNtNamePathStage;
    g_ipcState.ipc->hostNtHandlePathStage = g_captureState.hostNtHandlePathStage;
    g_ipcState.ipc->hostLegacyPathStage = g_captureState.hostLegacyPathStage;
    g_ipcState.ipc->resourceRequestId = requestId;
    g_ipcState.ipc->hostRequestedSharingMode = requestedMode;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        g_ipcState.ipc->sharedNames[slot][0] = L'\0';
        g_ipcState.ipc->sharedHandles[slot] = 0;
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[slot], 0);
    }
    InterlockedExchange(&g_ipcState.ipc->latestSlot, -1);
    EndMetadataWrite();

    SetMessage(requestedMode == FCS_SHARING_HOST_NT_NAME
        ? L"The game GPU device cannot create shared textures; preparing them in the preview instead..."
        : L"The named GPU-sharing path was rejected; preparing the compatibility ring instead...");
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_WAITING_HOST_RESOURCES);
}

namespace {

void RecordHostPathFailure(FcsSharingMode mode, HRESULT hr,
                           FcsResourceStage stage) {
    if (mode == FCS_SHARING_HOST_NT_NAME) {
        g_captureState.hostNtNamePathError = hr;
        g_captureState.hostNtNamePathStage = stage;
        g_captureState.hostNtHandlePathError = E_NOTIMPL;
        g_captureState.hostNtHandlePathStage = FCS_RESOURCE_STAGE_NONE;
        g_captureState.preferHostLegacy = true;
    } else {
        g_captureState.hostLegacyPathError = hr;
        g_captureState.hostLegacyPathStage = stage;
    }
}

bool HandleHostPathFailure(FcsSharingMode mode, HRESULT hr,
                           FcsResourceStage stage) {
    ReleaseSharedRing();
    RecordHostPathFailure(mode, hr, stage);
    if (mode == FCS_SHARING_HOST_NT_NAME) {
        PublishHostResourceRequest(FCS_SHARING_HOST_LEGACY_HANDLE);
        return false;
    }

    return ActivatePlainLegacyFallback();
}

bool RetryWithHostLegacy(HRESULT nameHr, FcsResourceStage nameStage,
                         HRESULT handleHr, FcsResourceStage handleStage) {
    ReleaseSharedRing();
    g_captureState.hostNtNamePathError = nameHr;
    g_captureState.hostNtNamePathStage = nameStage;
    g_captureState.hostNtHandlePathError = handleHr;
    g_captureState.hostNtHandlePathStage = handleStage;
    g_captureState.preferHostLegacy = true;
    PublishHostResourceRequest(FCS_SHARING_HOST_LEGACY_HANDLE);
    return false;
}

} // namespace

bool TryOpenHostOwnedRing() {
    if (!g_captureState.device || !g_captureState.activeHostRequestId || g_captureState.hostFallbackTerminal) return false;
    HostResourceResponse response{};
    if (!ReadHostResourceResponse(response) ||
        response.requestId != g_captureState.activeHostRequestId ||
        response.controllerPid != g_ipcState.ipc->controllerPid) {
        return false;
    }
    if (response.generation == g_captureState.lastHostAttemptGeneration) return false;
    g_captureState.lastHostAttemptGeneration = response.generation;

    const FcsSharingMode requestedMode = g_captureState.requestedHostMode;
    if (response.sharingMode != requestedMode) {
        return HandleHostPathFailure(requestedMode, E_INVALIDARG,
                                     requestedMode == FCS_SHARING_HOST_NT_NAME
                                         ? FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME
                                         : FCS_RESOURCE_STAGE_OPEN_SHARED_LEGACY);
    }
    if (FAILED(response.error)) {
        return HandleHostPathFailure(requestedMode, response.error,
                                     response.failureStage);
    }

    HRESULT hr = S_OK;
    FcsResourceStage failedStage = FCS_RESOURCE_STAGE_NONE;
    if (requestedMode == FCS_SHARING_HOST_NT_NAME) {
        ID3D11Device1* device1 = nullptr;
        const HRESULT device1Hr = g_captureState.device->QueryInterface(
            __uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
        if (FAILED(device1Hr) || !device1) {
            ReleaseCom(device1);
            return RetryWithHostLegacy(FAILED(device1Hr) ? device1Hr : E_NOINTERFACE,
                                       FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME,
                                       FAILED(device1Hr) ? device1Hr : E_NOINTERFACE,
                                       FCS_RESOURCE_STAGE_OPEN_SHARED_NT_HANDLE);
        }

        HRESULT nameHr = E_INVALIDARG;
        HRESULT handleHr = E_INVALIDARG;
        FcsResourceStage nameStage = FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME;
        FcsResourceStage handleStage = FCS_RESOURCE_STAGE_OPEN_SHARED_NT_HANDLE;
        const bool rejectAllNt =
            (g_ipcState.ipc->command & FCS_COMMAND_TEST_REJECT_HOST_NT) != 0;
        const bool rejectNtName =
            (g_ipcState.ipc->command & FCS_COMMAND_TEST_REJECT_HOST_NT_NAME) != 0;
        if (rejectAllNt) {
            InterlockedAnd(&g_ipcState.ipc->command, ~FCS_COMMAND_TEST_REJECT_HOST_NT);
        } else {
            if (rejectNtName) {
                InterlockedAnd(&g_ipcState.ipc->command,
                               ~FCS_COMMAND_TEST_REJECT_HOST_NT_NAME);
            } else {
                nameHr = OpenHostNtRingByName(device1, response, nameStage);
            }
            if (FAILED(nameHr)) {
                ReleaseSharedRing();
                handleHr = OpenHostNtRingByHandle(device1, response, handleStage);
            }
        }
        ReleaseCom(device1);

        if (SUCCEEDED(nameHr)) {
            g_captureState.hostNtNamePathError = S_OK;
            g_captureState.hostNtNamePathStage = FCS_RESOURCE_STAGE_NONE;
            g_captureState.hostNtHandlePathError = E_NOTIMPL;
            g_captureState.hostNtHandlePathStage = FCS_RESOURCE_STAGE_NONE;
            hr = S_OK;
        } else if (SUCCEEDED(handleHr)) {
            g_captureState.hostNtNamePathError = nameHr;
            g_captureState.hostNtNamePathStage = nameStage;
            g_captureState.hostNtHandlePathError = S_OK;
            g_captureState.hostNtHandlePathStage = FCS_RESOURCE_STAGE_NONE;
            hr = S_OK;
        } else {
            return RetryWithHostLegacy(nameHr, nameStage, handleHr, handleStage);
        }
    } else {
        if (g_ipcState.ipc->command & FCS_COMMAND_TEST_REJECT_HOST_LEGACY) {
            InterlockedAnd(&g_ipcState.ipc->command,
                           ~FCS_COMMAND_TEST_REJECT_HOST_LEGACY);
            hr = E_INVALIDARG;
            failedStage = FCS_RESOURCE_STAGE_OPEN_SHARED_LEGACY;
        } else {
            hr = OpenHostLegacyRing(response, failedStage);
        }
        if (SUCCEEDED(hr)) {
            g_captureState.hostLegacyPathError = S_OK;
            g_captureState.hostLegacyPathStage = FCS_RESOURCE_STAGE_NONE;
        }
    }
    if (FAILED(hr)) {
        return HandleHostPathFailure(requestedMode, hr, failedStage);
    }

    HostResourceResponse confirmed{};
    if (!ReadHostResourceResponse(confirmed) ||
        confirmed.generation != response.generation ||
        confirmed.requestId != response.requestId ||
        confirmed.controllerPid != response.controllerPid ||
        confirmed.controllerPid != g_ipcState.ipc->controllerPid ||
        confirmed.requestId != g_ipcState.ipc->resourceRequestId ||
        confirmed.sharingMode != response.sharingMode ||
        confirmed.sharingMode !=
            static_cast<FcsSharingMode>(g_ipcState.ipc->hostRequestedSharingMode) ||
        FAILED(confirmed.error)) {
        ReleaseSharedRing();
        return false;
    }

    g_captureState.sharingMode = requestedMode;
    BeginMetadataWrite();
    g_ipcState.ipc->sharingMode = requestedMode;
    g_ipcState.ipc->hostNtNamePathError = static_cast<LONG>(g_captureState.hostNtNamePathError);
    g_ipcState.ipc->hostNtHandlePathError = static_cast<LONG>(g_captureState.hostNtHandlePathError);
    g_ipcState.ipc->hostLegacyPathError = static_cast<LONG>(g_captureState.hostLegacyPathError);
    g_ipcState.ipc->hostNtNamePathStage = g_captureState.hostNtNamePathStage;
    g_ipcState.ipc->hostNtHandlePathStage = g_captureState.hostNtHandlePathStage;
    g_ipcState.ipc->hostLegacyPathStage = g_captureState.hostLegacyPathStage;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (requestedMode == FCS_SHARING_HOST_NT_NAME) {
            lstrcpynW(g_ipcState.ipc->sharedNames[slot], response.names[slot], 96);
            g_ipcState.ipc->sharedHandles[slot] = 0;
        } else {
            g_ipcState.ipc->sharedNames[slot][0] = L'\0';
            g_ipcState.ipc->sharedHandles[slot] = response.handles[slot];
        }
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[slot], 0);
    }
    InterlockedExchange(&g_ipcState.ipc->latestSlot, -1);
    EndMetadataWrite();
    InterlockedExchange(&g_ipcState.ipc->lastError, S_OK);
    SetMessage(requestedMode == FCS_SHARING_HOST_NT_NAME
        ? L"Clean frames are reaching the preview using controller-owned GPU sharing."
        : L"Clean frames are reaching the preview using controller-owned compatibility sharing.");
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_STREAMING);
    return true;
}

} // namespace fcs::hook
