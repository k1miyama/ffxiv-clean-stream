#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <wchar.h>

#include "../common/fcs_formats.hpp"
#include "fcs_com.hpp"
#include "fcs_preview.hpp"

namespace fcs::host {
namespace {

uint64_t PackLuid(const LUID& luid) {
    return static_cast<uint64_t>(static_cast<uint32_t>(luid.LowPart)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) <<
            32);
}

IDXGIAdapter1* FindAdapter(uint64_t packedLuid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(
            __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    IDXGIAdapter1* result = nullptr;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT enumResult = factory->EnumAdapters1(index, &adapter);
        if (enumResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumResult) || !adapter) {
            ReleaseCom(adapter);
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            PackLuid(desc.AdapterLuid) == packedLuid) {
            result = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}

} // namespace

PreviewRenderer::~PreviewRenderer() {
    Reset();
}

void PreviewRenderer::Reset() {
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        ReleaseCom(keyedMutexes_[i]);
        ReleaseCom(plainConsumerQueries_[i]);
        ReleaseCom(sharedTextures_[i]);
        if (sharedNtHandles_[i]) {
            CloseHandle(sharedNtHandles_[i]);
            sharedNtHandles_[i] = nullptr;
        }
        consumedSequence_[i] = 0;
        plainAckPending_[i] = false;
        plainPendingSequence_[i] = 0;
        plainPendingRingId_[i] = 0;
    }
    ReleaseCom(previewBuffer_);
    ReleaseCom(previewSwap_);
    ReleaseCom(context_);
    ReleaseCom(device1_);
    ReleaseCom(device_);
    openGeneration_ = -1;
    openResourceRequestId_ = 0;
    openSharingMode_ = FCS_SHARING_NONE;
    ownedHostRequestId_ = 0;
    ownedHostSharingMode_ = FCS_SHARING_NONE;
    lastPlainPreviewSequence_ = 0;
}

bool PreviewRenderer::SetupFailed(CaptureSession& session,
                                  const StableMetadata& metadata,
                                  StatusSink status,
                                  const wchar_t* text) {
    // Resource setup is retried through the compatibility ladder. Keep this
    // as progress until the controller observes the terminal failure count.
    ReportStatus(status, text);
    if (session.Data() && ++viewerFailureCount_ <= 3) {
        session.RequestResourceRecreate();
    }
    Reset();
    openGeneration_ = metadata.generation;
    return false;
}

bool PreviewRenderer::CreateHostOwnedViewer(
    CaptureSession& session, const StableMetadata& metadata,
    StatusSink status) {
    Reset();
    HostResourceResponse response{};
    response.requestId = metadata.resourceRequestId;
    response.sharingMode = metadata.requestedHostMode;
    HRESULT hr = S_OK;
    FcsResourceStage failureStage = FCS_RESOURCE_STAGE_NONE;
    const FcsSharingMode requestedMode = metadata.requestedHostMode;
    if (requestedMode != FCS_SHARING_HOST_NT_NAME &&
        requestedMode != FCS_SHARING_HOST_LEGACY_HANDLE) {
        hr = E_INVALIDARG;
        failureStage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
    }

    const DXGI_FORMAT previewFormat = FcsPreviewFormat(metadata.format);
    IDXGIAdapter1* adapter = FAILED(hr) ||
            previewFormat == DXGI_FORMAT_UNKNOWN
        ? nullptr
        : FindAdapter(metadata.adapterLuid);
    if (SUCCEEDED(hr) && !adapter) {
        hr = DXGI_ERROR_NOT_FOUND;
        failureStage = FCS_RESOURCE_STAGE_FIND_ADAPTER;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = metadata.width;
    swapDesc.BufferDesc.Height = metadata.height;
    swapDesc.BufferDesc.Format = previewFormat;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.OutputWindow = outputWindow_;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level{};
    if (SUCCEEDED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &swapDesc, &previewSwap_, &device_, &level, &context_);
        if (FAILED(hr)) failureStage = FCS_RESOURCE_STAGE_CREATE_DEVICE;
    }
    ReleaseCom(adapter);
    if (SUCCEEDED(hr)) {
        hr = previewSwap_->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&previewBuffer_));
        if (FAILED(hr)) {
            failureStage = FCS_RESOURCE_STAGE_GET_PREVIEW_BUFFER;
        }
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = metadata.width;
    textureDesc.Height = metadata.height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = metadata.format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = requestedMode == FCS_SHARING_HOST_NT_NAME
        ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
              D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
        : D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (UINT slot = 0; SUCCEEDED(hr) && slot < FCS_SLOT_COUNT; ++slot) {
        hr = device_->CreateTexture2D(
            &textureDesc, nullptr, &sharedTextures_[slot]);
        if (FAILED(hr)) {
            failureStage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
            break;
        }
        hr = sharedTextures_[slot]->QueryInterface(
            __uuidof(IDXGIKeyedMutex),
            reinterpret_cast<void**>(&keyedMutexes_[slot]));
        if (FAILED(hr)) {
            failureStage = FCS_RESOURCE_STAGE_KEYED_MUTEX;
            break;
        }
        if (requestedMode == FCS_SHARING_HOST_NT_NAME) {
            IDXGIResource1* resource = nullptr;
            hr = sharedTextures_[slot]->QueryInterface(
                __uuidof(IDXGIResource1),
                reinterpret_cast<void**>(&resource));
            if (FAILED(hr)) {
                failureStage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
                ReleaseCom(resource);
                break;
            }
            swprintf_s(response.names[slot], 96,
                       L"Local\\FCS5.HostTexture.%lu.%I64x.%u",
                       static_cast<unsigned long>(GetCurrentProcessId()),
                       static_cast<unsigned long long>(
                           metadata.resourceRequestId),
                       slot);
            hr = resource->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                response.names[slot], &sharedNtHandles_[slot]);
            ReleaseCom(resource);
            if (SUCCEEDED(hr) && !sharedNtHandles_[slot]) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                response.handles[slot] = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(sharedNtHandles_[slot]));
            }
        } else {
            IDXGIResource* resource = nullptr;
            hr = sharedTextures_[slot]->QueryInterface(
                __uuidof(IDXGIResource),
                reinterpret_cast<void**>(&resource));
            if (FAILED(hr)) {
                failureStage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
                ReleaseCom(resource);
                break;
            }
            HANDLE sharedHandle = nullptr;
            hr = resource->GetSharedHandle(&sharedHandle);
            ReleaseCom(resource);
            if (SUCCEEDED(hr) && !sharedHandle) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                // Legacy DXGI handles are opaque cross-process values. Keep
                // the creator texture alive and never CloseHandle the value.
                response.handles[slot] = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(sharedHandle));
            }
        }
        if (FAILED(hr)) failureStage = FCS_RESOURCE_STAGE_CREATE_HANDLE;
    }

    response.error = hr;
    response.failureStage = failureStage;
    if (FAILED(hr)) {
        const LONG64 failedRequest = metadata.resourceRequestId;
        Reset();
        failedHostRequestId_ = failedRequest;
        session.PublishHostResponse(response);
        ReportStatus(
            status,
            L"The preview GPU could not create the compatibility frame ring.");
        return false;
    }

    ownedHostRequestId_ = metadata.resourceRequestId;
    ownedHostSharingMode_ = requestedMode;
    failedHostRequestId_ = 0;
    if (!session.PublishHostResponse(response)) {
        Reset();
        return false;
    }
    ReportStatus(
        status, requestedMode == FCS_SHARING_HOST_NT_NAME
            ? L"GPU sharing is ready; waiting for FFXIV to open it..."
            : L"Compatibility GPU sharing is ready; waiting for FFXIV to open it...");
    return true;
}

void PreviewRenderer::ServiceHostResourceRequest(CaptureSession& session,
                                                 StatusSink status) {
    FcsIpcV1* ipc = session.Data();
    if (!ipc || ipc->controllerPid != GetCurrentProcessId()) return;
    StableMetadata metadata{};
    if (!session.ReadStableMetadata(metadata) ||
        metadata.sharingMode != FCS_SHARING_HOST_REQUEST) {
        return;
    }
    if (metadata.resourceRequestId == ownedHostRequestId_ &&
        metadata.requestedHostMode == ownedHostSharingMode_ && device_ &&
        previewSwap_ && previewBuffer_) {
        return;
    }
    if (metadata.resourceRequestId == failedHostRequestId_) return;
    CreateHostOwnedViewer(session, metadata, status);
}

bool PreviewRenderer::CreateViewerForMetadata(
    CaptureSession& session, const StableMetadata& metadata,
    StatusSink status) {
    Reset();
    const DXGI_FORMAT previewFormat = FcsPreviewFormat(metadata.format);
    if (previewFormat == DXGI_FORMAT_UNKNOWN) {
        return SetupFailed(
            session, metadata, status,
            L"The preview does not support this game format.");
    }
    IDXGIAdapter1* adapter = FindAdapter(metadata.adapterLuid);
    if (!adapter) {
        return SetupFailed(
            session, metadata, status,
            L"The preview could not find the same GPU used by FFXIV.");
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = metadata.width;
    desc.BufferDesc.Height = metadata.height;
    desc.BufferDesc.Format = previewFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = outputWindow_;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &previewSwap_, &device_, &level, &context_);
    adapter->Release();
    if (FAILED(hr)) {
        return SetupFailed(
            session, metadata, status,
            L"The clean preview swap chain could not be created for this game format.");
    }
    hr = device_->QueryInterface(
        __uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1_));
    if (FAILED(hr) &&
        (metadata.sharingMode == FCS_SHARING_NT_NAME ||
         metadata.sharingMode == FCS_SHARING_HOST_NT_NAME)) {
        return SetupFailed(
            session, metadata, status,
            L"Direct3D 11.1 GPU sharing is unavailable on this system.");
    }
    hr = previewSwap_->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&previewBuffer_));
    if (FAILED(hr)) {
        return SetupFailed(
            session, metadata, status,
            L"The clean preview backbuffer could not be opened.");
    }

    HRESULT openResult = S_OK;
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        if (SUCCEEDED(openResult)) {
            if (metadata.sharingMode == FCS_SHARING_NT_NAME ||
                metadata.sharingMode == FCS_SHARING_HOST_NT_NAME) {
                openResult = device1_->OpenSharedResourceByName(
                    metadata.names[i],
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&sharedTextures_[i]));
            } else {
                const HANDLE sharedHandle = reinterpret_cast<HANDLE>(
                    static_cast<uintptr_t>(metadata.handles[i]));
                openResult = device_->OpenSharedResource(
                    sharedHandle, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&sharedTextures_[i]));
            }
        }
        if (SUCCEEDED(openResult) &&
            metadata.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE) {
            openResult = sharedTextures_[i]->QueryInterface(
                __uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&keyedMutexes_[i]));
        }
        if (SUCCEEDED(openResult) &&
            metadata.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE) {
            D3D11_TEXTURE2D_DESC openedDesc{};
            sharedTextures_[i]->GetDesc(&openedDesc);
            if (openedDesc.Width != metadata.width ||
                openedDesc.Height != metadata.height ||
                openedDesc.MipLevels != 1 || openedDesc.ArraySize != 1 ||
                openedDesc.Format != metadata.format ||
                openedDesc.SampleDesc.Count != 1 ||
                openedDesc.Usage != D3D11_USAGE_DEFAULT ||
                openedDesc.CPUAccessFlags != 0 ||
                (openedDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0 ||
                (openedDesc.MiscFlags &
                 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0) {
                openResult = E_INVALIDARG;
            } else {
                D3D11_QUERY_DESC queryDesc{};
                queryDesc.Query = D3D11_QUERY_EVENT;
                openResult = device_->CreateQuery(
                    &queryDesc, &plainConsumerQueries_[i]);
            }
        }
    }
    if (FAILED(openResult)) {
        return SetupFailed(
            session, metadata, status,
            L"A shared clean-frame surface could not be opened. Click Start / Resume.");
    }
    viewerFailureCount_ = 0;
    openGeneration_ = metadata.generation;
    openResourceRequestId_ = metadata.resourceRequestId;
    openSharingMode_ = metadata.sharingMode;
    ShowWindow(outputWindow_, SW_SHOWNORMAL);
    return true;
}

bool PreviewRenderer::EnsureViewer(CaptureSession& session,
                                   StatusSink status) {
    StableMetadata metadata{};
    if (!session.ReadStableMetadata(metadata)) return false;
    // HOST_REQUEST is a producer-to-controller handshake, not a consumable
    // resource set. ServiceHostResourceRequest owns this transition. In
    // particular, do not send its intentionally empty handles through the
    // normal viewer-opening path during the brief STREAMING -> WAITING state
    // transition on resize/reconnect.
    if (metadata.sharingMode == FCS_SHARING_HOST_REQUEST) return false;
    if (metadata.generation == openGeneration_) return device_ != nullptr;
    const bool hostOwnedMode =
        metadata.sharingMode == FCS_SHARING_HOST_NT_NAME ||
        metadata.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE;
    if (hostOwnedMode &&
        metadata.resourceRequestId == ownedHostRequestId_ &&
        metadata.sharingMode == ownedHostSharingMode_ && device_ &&
        previewSwap_ && previewBuffer_) {
        openGeneration_ = metadata.generation;
        openResourceRequestId_ = metadata.resourceRequestId;
        openSharingMode_ = metadata.sharingMode;
        viewerFailureCount_ = 0;
        ShowWindow(outputWindow_, SW_SHOWNORMAL);
        return true;
    }
    if (hostOwnedMode) {
        ReportStatus(
            status,
            L"Waiting for FFXIV to request a new controller-owned frame ring...");
        session.RequestResourceRecreate();
        return false;
    }
    return CreateViewerForMetadata(session, metadata, status);
}

} // namespace fcs::host
