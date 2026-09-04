#include "fcs_internal.hpp"
#include "fcs_recreate_policy.hpp"

namespace fcs::hook {
namespace {

void SetResourceError(HRESULT hr, const wchar_t* stage,
                      const D3D11_TEXTURE2D_DESC& sourceDesc,
                      DXGI_FORMAT transportFormat) {
    wchar_t text[256]{};
    swprintf_s(text, sizeof(text) / sizeof(text[0]),
               L"Shared capture setup failed at %ls (0x%08lX). "
               L"Source format %u -> transport %u, %ux%u, samples %u, bind flags 0x%X.",
               stage, static_cast<unsigned long>(static_cast<uint32_t>(hr)),
               static_cast<unsigned int>(sourceDesc.Format),
               static_cast<unsigned int>(transportFormat),
               static_cast<unsigned int>(sourceDesc.Width),
               static_cast<unsigned int>(sourceDesc.Height),
               static_cast<unsigned int>(sourceDesc.SampleDesc.Count),
               static_cast<unsigned int>(D3D11_BIND_SHADER_RESOURCE |
                                         D3D11_BIND_RENDER_TARGET));
    SetError(static_cast<LONG>(hr), text);
}

uint64_t PackLuid(const LUID& luid) {
    return static_cast<uint64_t>(static_cast<uint32_t>(luid.LowPart)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
}

} // namespace

void ReleaseCaptureResources() {
    ReleaseSharedRing();
    ReleaseCom(g_captureState.context);
    ReleaseCom(g_captureState.device);
    g_captureState.width = 0;
    g_captureState.height = 0;
    g_captureState.sourceFormat = DXGI_FORMAT_UNKNOWN;
    g_captureState.transportFormat = DXGI_FORMAT_UNKNOWN;
    g_captureState.sourceSamples = 1;
    g_captureState.resourceSwap = nullptr;
    g_captureState.nextSlot = 0;
    g_captureState.sharingMode = FCS_SHARING_NONE;
    g_captureState.requestedHostMode = FCS_SHARING_NONE;
    g_captureState.activeHostRequestId = 0;
    g_captureState.lastHostAttemptGeneration = -1;
    g_captureState.adapterLuid = 0;
    g_captureState.hostFallbackTerminal = false;
}

namespace {

bool PublishNewResources(IDXGISwapChain* swap, const D3D11_TEXTURE2D_DESC& sourceDesc) {
    ReleaseCaptureResources();

    const DXGI_FORMAT transportFormat = FcsTransportFormat(sourceDesc.Format);
    if (transportFormat == DXGI_FORMAT_UNKNOWN) {
        SetResourceError(DXGI_ERROR_UNSUPPORTED, L"format selection", sourceDesc,
                         transportFormat);
        return false;
    }

    HRESULT hr = swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_captureState.device));
    if (FAILED(hr) || !g_captureState.device) {
        SetError(hr, L"Could not get FFXIV's Direct3D 11 device.");
        return false;
    }
    g_captureState.device->GetImmediateContext(&g_captureState.context);
    if (!g_captureState.context) {
        SetError(E_FAIL, L"Could not get FFXIV's Direct3D context.");
        ReleaseCaptureResources();
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapterDesc{};
    hr = g_captureState.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapterDesc);
    ReleaseCom(adapter);
    ReleaseCom(dxgiDevice);
    if (FAILED(hr)) {
        SetError(hr, L"Could not identify the GPU used by FFXIV.");
        ReleaseCaptureResources();
        return false;
    }

    g_captureState.width = sourceDesc.Width;
    g_captureState.height = sourceDesc.Height;
    g_captureState.sourceFormat = sourceDesc.Format;
    g_captureState.transportFormat = transportFormat;
    g_captureState.sourceSamples = sourceDesc.SampleDesc.Count;
    g_captureState.resourceSwap = swap;
    g_captureState.adapterLuid = PackLuid(adapterDesc.AdapterLuid);
    g_captureState.hostFallbackTerminal = false;
    g_captureState.hostLegacyPathError = E_PENDING;
    g_captureState.hostLegacyPathStage = FCS_RESOURCE_STAGE_NONE;
    g_captureState.plainCreateError = E_PENDING;
    g_captureState.plainCreateStage = FCS_RESOURCE_STAGE_NONE;
    if (!g_captureState.preferHostLegacy) {
        g_captureState.hostNtNamePathError = E_PENDING;
        g_captureState.hostNtHandlePathError = E_PENDING;
        g_captureState.hostNtNamePathStage = FCS_RESOURCE_STAGE_NONE;
        g_captureState.hostNtHandlePathStage = FCS_RESOURCE_STAGE_NONE;
    }

    D3D11_TEXTURE2D_DESC sharedDesc = sourceDesc;
    sharedDesc.MipLevels = 1;
    sharedDesc.ArraySize = 1;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.SampleDesc.Quality = 0;
    sharedDesc.Format = transportFormat;
    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sharedDesc.CPUAccessFlags = 0;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                           D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    if (g_captureState.preferPlainLegacy) {
        return ActivatePlainLegacyFallback();
    }

    wchar_t resourceNames[FCS_SLOT_COUNT][96]{};
    uint64_t legacyHandles[FCS_SLOT_COUNT]{};
    FcsSharingMode sharingMode = FCS_SHARING_NONE;
    const bool forceHostResources =
        (g_ipcState.ipc->command & FCS_COMMAND_FORCE_HOST_RESOURCES) != 0;
    g_captureState.ntCreateStage = FCS_RESOURCE_STAGE_NONE;
    g_captureState.legacyCreateStage = FCS_RESOURCE_STAGE_NONE;
    g_captureState.legacyCreateError = S_OK;
    g_captureState.ntCreateError = forceHostResources ? E_NOTIMPL :
        CreateNtSharedRing(sharedDesc, resourceNames, g_captureState.ntCreateStage);
    if (SUCCEEDED(g_captureState.ntCreateError)) {
        sharingMode = FCS_SHARING_NT_NAME;
    } else {
        ReleaseSharedRing();
        g_captureState.legacyCreateError = forceHostResources ? E_NOTIMPL :
            CreateLegacySharedRing(sharedDesc, legacyHandles, g_captureState.legacyCreateStage);
        if (SUCCEEDED(g_captureState.legacyCreateError)) {
            sharingMode = FCS_SHARING_LEGACY_HANDLE;
        } else {
            ReleaseSharedRing();
            PublishHostResourceRequest(g_captureState.preferHostLegacy
                ? FCS_SHARING_HOST_LEGACY_HANDLE
                : FCS_SHARING_HOST_NT_NAME);
            return false;
        }
    }
    if (sharingMode == FCS_SHARING_NONE) {
        SetResourceError(E_FAIL, L"sharing-mode selection", sourceDesc, transportFormat);
        ReleaseCaptureResources();
        return false;
    }

    g_captureState.sharingMode = sharingMode;

    BeginMetadataWrite();
    g_ipcState.ipc->producerPid = GetCurrentProcessId();
    g_ipcState.ipc->adapterLuid = PackLuid(adapterDesc.AdapterLuid);
    g_ipcState.ipc->width = g_captureState.width;
    g_ipcState.ipc->height = g_captureState.height;
    g_ipcState.ipc->format = static_cast<uint32_t>(g_captureState.transportFormat);
    g_ipcState.ipc->sampleCount = g_captureState.sourceSamples;
    g_ipcState.ipc->sharingMode = sharingMode;
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
    g_ipcState.ipc->hostRequestedSharingMode = FCS_SHARING_NONE;
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        if (sharingMode == FCS_SHARING_NT_NAME) {
            lstrcpynW(g_ipcState.ipc->sharedNames[i], resourceNames[i], 96);
            g_ipcState.ipc->sharedHandles[i] = 0;
        } else {
            g_ipcState.ipc->sharedNames[i][0] = L'\0';
            g_ipcState.ipc->sharedHandles[i] = legacyHandles[i];
        }
        InterlockedExchange64(&g_ipcState.ipc->slotSequence[i], 0);
    }
    InterlockedExchange(&g_ipcState.ipc->latestSlot, -1);
    EndMetadataWrite();

    SetMessage(sharingMode == FCS_SHARING_NT_NAME
                   ? L"Clean frames are reaching the standalone preview."
                   : L"Clean frames are reaching the standalone preview using compatibility GPU sharing.");
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_STREAMING);
    return true;
}

} // namespace

bool EnsureCaptureResources(IDXGISwapChain* swap, const D3D11_TEXTURE2D_DESC& desc) {
    if (InterlockedExchange(&g_captureState.sourceChanged, 0)) {
        InvalidatePublishedResources();
        ReleaseCaptureResources();
    }
    const bool recreatePending =
        (g_ipcState.ipc->command & FCS_COMMAND_RECREATE_RESOURCES) != 0;
    if (recreatePending) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // The device is the owning root of the capture resource graph. A
        // stopped capture releases it before a rapid resume can arrive.
        const ExternalRecreateDecision decision = DecideExternalRecreate(
            recreatePending, g_captureState.device != nullptr, now.QuadPart,
            g_captureState.nextExternalRecreateQpc);
        if (decision.consume) {
            InterlockedAnd(&g_ipcState.ipc->command, ~FCS_COMMAND_RECREATE_RESOURCES);
            g_captureState.nextExternalRecreateQpc = now.QuadPart + (g_captureState.qpcFrequency.QuadPart * 2);
            g_captureState.resourceRetryAfterQpc = 0;
            // An explicit resume/reconnect is a new attempt, so revisit the
            // NT paths in case their earlier failure was transient. Automatic
            // resize recovery keeps the proven legacy preference.
            g_captureState.preferHostLegacy = false;
            g_captureState.preferPlainLegacy = false;
            g_captureState.hostFallbackTerminal = false;
            if (decision.releaseLiveGraph) {
                InvalidatePublishedResources();
                ReleaseCaptureResources();
            }
        }
    }
    bool sameDevice = false;
    if (g_captureState.device) {
        ID3D11Device* currentDevice = nullptr;
        if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D11Device),
                                      reinterpret_cast<void**>(&currentDevice)))) {
            sameDevice = currentDevice == g_captureState.device;
        }
        ReleaseCom(currentDevice);
    }
    if (sameDevice && g_captureState.resourceSwap == swap && g_captureState.width == desc.Width && g_captureState.height == desc.Height &&
        g_captureState.sourceFormat == desc.Format && g_captureState.sourceSamples == desc.SampleDesc.Count) {
        if (g_captureState.sharingMode == FCS_SHARING_HOST_REQUEST) {
            return TryOpenHostOwnedRing();
        }
        return g_captureState.sharingMode == FCS_SHARING_NT_NAME ||
               g_captureState.sharingMode == FCS_SHARING_LEGACY_HANDLE ||
               g_captureState.sharingMode == FCS_SHARING_HOST_NT_NAME ||
               g_captureState.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE ||
               g_captureState.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE;
    }
    InvalidatePublishedResources();
    if (PublishNewResources(swap, desc)) return true;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_captureState.resourceRetryAfterQpc = now.QuadPart +
        (g_captureState.sharingMode == FCS_SHARING_HOST_REQUEST
             ? (g_captureState.qpcFrequency.QuadPart / 100)
             : (g_captureState.qpcFrequency.QuadPart * 2));
    return false;
}

} // namespace fcs::hook
