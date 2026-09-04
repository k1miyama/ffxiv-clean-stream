#include "fcs_internal.hpp"

namespace fcs::hook {

void ReleaseSharedRing() {
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        if (g_captureState.sharedNtHandles[i]) {
            CloseHandle(g_captureState.sharedNtHandles[i]);
            g_captureState.sharedNtHandles[i] = nullptr;
        }
        ReleaseCom(g_captureState.keyedMutexes[i]);
        ReleaseCom(g_captureState.plainProducerQueries[i]);
        ReleaseCom(g_captureState.sharedTextures[i]);
        g_captureState.plainSlotStates[i] = PLAIN_SLOT_FREE;
        g_captureState.plainSlotSequences[i] = 0;
        g_captureState.plainSlotRingIds[i] = 0;
        g_captureState.plainSlotQpc[i] = 0;
    }
}

HRESULT CreatePlainLegacySharedRing(const D3D11_TEXTURE2D_DESC& baseDesc,
                                    uint64_t (&sharedHandles)[FCS_SLOT_COUNT],
                                    FcsResourceStage& failedStage) {
    D3D11_TEXTURE2D_DESC sharedDesc = baseDesc;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    D3D11_QUERY_DESC queryDesc{};
    queryDesc.Query = D3D11_QUERY_EVENT;

    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        HRESULT hr = g_captureState.device->CreateTexture2D(
            &sharedDesc, nullptr, &g_captureState.sharedTextures[slot]);
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
            return hr;
        }

        IDXGIResource* resource = nullptr;
        hr = g_captureState.sharedTextures[slot]->QueryInterface(
            __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
            ReleaseCom(resource);
            return hr;
        }
        HANDLE sharedHandle = nullptr;
        hr = resource->GetSharedHandle(&sharedHandle);
        ReleaseCom(resource);
        if (FAILED(hr) || !sharedHandle) {
            if (SUCCEEDED(hr)) hr = E_FAIL;
            failedStage = FCS_RESOURCE_STAGE_CREATE_HANDLE;
            return hr;
        }
        sharedHandles[slot] = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(sharedHandle));

        hr = g_captureState.device->CreateQuery(&queryDesc, &g_captureState.plainProducerQueries[slot]);
        if (FAILED(hr) || !g_captureState.plainProducerQueries[slot]) {
            if (SUCCEEDED(hr)) hr = E_FAIL;
            failedStage = FCS_RESOURCE_STAGE_CREATE_QUERY;
            return hr;
        }
    }
    return S_OK;
}

HRESULT CreateNtSharedRing(const D3D11_TEXTURE2D_DESC& sharedDesc,
                           wchar_t (&resourceNames)[FCS_SLOT_COUNT][96],
                           FcsResourceStage& failedStage) {
    HRESULT hr = S_OK;
    LARGE_INTEGER nonce{};
    QueryPerformanceCounter(&nonce);
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        hr = g_captureState.device->CreateTexture2D(&sharedDesc, nullptr, &g_captureState.sharedTextures[i]);
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
            return hr;
        }

        hr = g_captureState.sharedTextures[i]->QueryInterface(
            __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&g_captureState.keyedMutexes[i]));
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_KEYED_MUTEX;
            return hr;
        }

        IDXGIResource1* resource = nullptr;
        hr = g_captureState.sharedTextures[i]->QueryInterface(
            __uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource));
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
            ReleaseCom(resource);
            return hr;
        }
        swprintf_s(resourceNames[i], 96, L"Local\\FCS5.Texture.%lu.%I64x.%u",
                   static_cast<unsigned long>(GetCurrentProcessId()),
                   static_cast<unsigned long long>(nonce.QuadPart), i);
        hr = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            resourceNames[i], &g_captureState.sharedNtHandles[i]);
        ReleaseCom(resource);
        if (FAILED(hr) || !g_captureState.sharedNtHandles[i]) {
            if (SUCCEEDED(hr)) hr = E_FAIL;
            failedStage = FCS_RESOURCE_STAGE_CREATE_HANDLE;
            return hr;
        }
    }
    return S_OK;
}

HRESULT CreateLegacySharedRing(const D3D11_TEXTURE2D_DESC& baseDesc,
                               uint64_t (&sharedHandles)[FCS_SLOT_COUNT],
                               FcsResourceStage& failedStage) {
    D3D11_TEXTURE2D_DESC sharedDesc = baseDesc;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HRESULT hr = S_OK;
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        hr = g_captureState.device->CreateTexture2D(&sharedDesc, nullptr, &g_captureState.sharedTextures[i]);
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
            return hr;
        }

        hr = g_captureState.sharedTextures[i]->QueryInterface(
            __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&g_captureState.keyedMutexes[i]));
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_KEYED_MUTEX;
            return hr;
        }

        IDXGIResource* resource = nullptr;
        hr = g_captureState.sharedTextures[i]->QueryInterface(
            __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
        if (FAILED(hr)) {
            failedStage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
            ReleaseCom(resource);
            return hr;
        }
        HANDLE sharedHandle = nullptr;
        hr = resource->GetSharedHandle(&sharedHandle);
        ReleaseCom(resource);
        if (FAILED(hr) || !sharedHandle) {
            if (SUCCEEDED(hr)) hr = E_FAIL;
            failedStage = FCS_RESOURCE_STAGE_CREATE_HANDLE;
            return hr;
        }
        // This is an opaque DXGI shared-resource value, not an NT handle.
        sharedHandles[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
    }
    return S_OK;
}

namespace {

HRESULT FinishOpenedHostSlot(UINT slot, FcsResourceStage openStage,
                             FcsResourceStage& failedStage) {
    if (!g_captureState.sharedTextures[slot]) return E_FAIL;
    D3D11_TEXTURE2D_DESC openedDesc{};
    g_captureState.sharedTextures[slot]->GetDesc(&openedDesc);
    if (openedDesc.Width != g_captureState.width || openedDesc.Height != g_captureState.height ||
        openedDesc.MipLevels != 1 || openedDesc.ArraySize != 1 ||
        openedDesc.Format != g_captureState.transportFormat ||
        openedDesc.SampleDesc.Count != 1 ||
        openedDesc.Usage != D3D11_USAGE_DEFAULT ||
        (openedDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) == 0) {
        failedStage = openStage;
        return E_INVALIDARG;
    }
    const HRESULT hr = g_captureState.sharedTextures[slot]->QueryInterface(
        __uuidof(IDXGIKeyedMutex),
        reinterpret_cast<void**>(&g_captureState.keyedMutexes[slot]));
    if (FAILED(hr)) failedStage = FCS_RESOURCE_STAGE_KEYED_MUTEX;
    return hr;
}

} // namespace

HRESULT OpenHostNtRingByName(ID3D11Device1* device1,
                             const HostResourceResponse& response,
                             FcsResourceStage& failedStage) {
    failedStage = FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (!response.names[slot][0]) return E_INVALIDARG;
        HRESULT hr = device1->OpenSharedResourceByName(
            response.names[slot],
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&g_captureState.sharedTextures[slot]));
        if (FAILED(hr)) return hr;
        hr = FinishOpenedHostSlot(slot, FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME,
                                  failedStage);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT OpenHostNtRingByHandle(ID3D11Device1* device1,
                               const HostResourceResponse& response,
                               FcsResourceStage& failedStage) {
    failedStage = FCS_RESOURCE_STAGE_DUPLICATE_NT_HANDLE;
    HANDLE controller = OpenProcess(PROCESS_DUP_HANDLE, FALSE, response.controllerPid);
    if (!controller) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr = S_OK;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (!response.handles[slot]) {
            hr = E_INVALIDARG;
            break;
        }
        HANDLE localHandle = nullptr;
        if (!DuplicateHandle(controller,
                             reinterpret_cast<HANDLE>(
                                 static_cast<uintptr_t>(response.handles[slot])),
                             GetCurrentProcess(), &localHandle, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        failedStage = FCS_RESOURCE_STAGE_OPEN_SHARED_NT_HANDLE;
        hr = device1->OpenSharedResource1(
            localHandle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&g_captureState.sharedTextures[slot]));
        CloseHandle(localHandle);
        if (FAILED(hr)) break;
        hr = FinishOpenedHostSlot(slot, FCS_RESOURCE_STAGE_OPEN_SHARED_NT_HANDLE,
                                  failedStage);
        if (FAILED(hr)) break;
        failedStage = FCS_RESOURCE_STAGE_DUPLICATE_NT_HANDLE;
    }
    CloseHandle(controller);
    return hr;
}

HRESULT OpenHostLegacyRing(const HostResourceResponse& response,
                           FcsResourceStage& failedStage) {
    failedStage = FCS_RESOURCE_STAGE_OPEN_SHARED_LEGACY;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (!response.handles[slot]) return E_INVALIDARG;
        const HANDLE sharedHandle = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(response.handles[slot]));
        HRESULT hr = g_captureState.device->OpenSharedResource(
            sharedHandle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&g_captureState.sharedTextures[slot]));
        if (FAILED(hr)) return hr;
        hr = FinishOpenedHostSlot(slot, FCS_RESOURCE_STAGE_OPEN_SHARED_LEGACY,
                                  failedStage);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

} // namespace fcs::hook
