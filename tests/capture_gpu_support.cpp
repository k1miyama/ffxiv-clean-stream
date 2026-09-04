#include "capture_e2e_support_internal.hpp"

#include <stdio.h>
#include <wchar.h>

namespace fcs_test {

static uint64_t PackLuid(const LUID& luid) {
    return static_cast<uint64_t>(static_cast<uint32_t>(luid.LowPart)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
}

IDXGIAdapter1* FindAdapter(uint64_t packedLuid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) return nullptr;
    IDXGIAdapter1* found = nullptr;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && PackLuid(desc.AdapterLuid) == packedLuid) {
            found = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

struct ForcedHostRing {
    LONG64 requestId;
    FcsSharingMode sharingMode;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* textures[FCS_SLOT_COUNT];
    IDXGIKeyedMutex* mutexes[FCS_SLOT_COUNT];
    HANDLE ntHandles[FCS_SLOT_COUNT];
    uint64_t publishedHandles[FCS_SLOT_COUNT];
};

static ForcedHostRing g_forcedHostRing{};
static UINT g_forcedHostRequestCount = 0;
static bool g_sawHostNtRequest = false;
static bool g_sawHostLegacyRequest = false;

void DestroyForcedHostRing() {
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        ReleaseCom(g_forcedHostRing.mutexes[slot]);
        ReleaseCom(g_forcedHostRing.textures[slot]);
        if (g_forcedHostRing.ntHandles[slot]) {
            CloseHandle(g_forcedHostRing.ntHandles[slot]);
            g_forcedHostRing.ntHandles[slot] = nullptr;
        }
        g_forcedHostRing.publishedHandles[slot] = 0;
    }
    ReleaseCom(g_forcedHostRing.context);
    ReleaseCom(g_forcedHostRing.device);
    g_forcedHostRing.requestId = 0;
    g_forcedHostRing.sharingMode = FCS_SHARING_NONE;
}

UINT ForcedHostRequestCount() {
    return g_forcedHostRequestCount;
}

bool SawHostNtRequest() {
    return g_sawHostNtRequest;
}

bool SawHostLegacyRequest() {
    return g_sawHostLegacyRequest;
}

static void PublishForcedHostResponse(FcsIpcV1* ipc, LONG64 requestId,
                               FcsSharingMode sharingMode, HRESULT error,
                               FcsResourceStage stage,
                               const wchar_t (&names)[FCS_SLOT_COUNT][96],
                               const uint64_t (&handles)[FCS_SLOT_COUNT]) {
    LONG64 generation = InterlockedIncrement64(&ipc->hostGeneration);
    if ((generation & 1) == 0) InterlockedIncrement64(&ipc->hostGeneration);
    MemoryBarrier();
    ipc->hostRequestId = requestId;
    ipc->hostControllerPid = GetCurrentProcessId();
    ipc->hostResourceError = static_cast<LONG>(error);
    ipc->hostFailureStage = stage;
    ipc->hostResponseSharingMode = sharingMode;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        lstrcpynW(ipc->hostSharedNames[slot], names[slot], 96);
        ipc->hostSharedHandles[slot] = handles[slot];
    }
    MemoryBarrier();
    generation = InterlockedIncrement64(&ipc->hostGeneration);
    if ((generation & 1) != 0) InterlockedIncrement64(&ipc->hostGeneration);
}

bool detail::ServiceForcedHostResources(FcsIpcV1* ipc) {
    if (!ipc) return true;
    StableMetadata metadata{};
    if (!ReadStableMetadata(ipc, metadata) ||
        metadata.sharingMode != FCS_SHARING_HOST_REQUEST) return true;
    if (g_forcedHostRing.requestId == metadata.resourceRequestId) return true;

    DestroyForcedHostRing();
    wchar_t names[FCS_SLOT_COUNT][96]{};
    uint64_t handles[FCS_SLOT_COUNT]{};
    HRESULT hr = S_OK;
    FcsResourceStage stage = FCS_RESOURCE_STAGE_NONE;
    const FcsSharingMode requestedMode = metadata.requestedHostMode;
    ++g_forcedHostRequestCount;
    if (requestedMode == FCS_SHARING_HOST_NT_NAME) g_sawHostNtRequest = true;
    if (requestedMode == FCS_SHARING_HOST_LEGACY_HANDLE) g_sawHostLegacyRequest = true;
    if (requestedMode != FCS_SHARING_HOST_NT_NAME &&
        requestedMode != FCS_SHARING_HOST_LEGACY_HANDLE) {
        hr = E_INVALIDARG;
        stage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
    }
    IDXGIAdapter1* adapter = FindAdapter(metadata.adapterLuid);
    if (SUCCEEDED(hr) && !adapter) {
        hr = DXGI_ERROR_NOT_FOUND;
        stage = FCS_RESOURCE_STAGE_FIND_ADAPTER;
    }
    if (SUCCEEDED(hr)) {
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION,
                               &g_forcedHostRing.device, nullptr,
                               &g_forcedHostRing.context);
        if (FAILED(hr)) stage = FCS_RESOURCE_STAGE_CREATE_DEVICE;
    }
    ReleaseCom(adapter);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = metadata.width;
    desc.Height = metadata.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = metadata.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = requestedMode == FCS_SHARING_HOST_NT_NAME
        ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
              D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
        : D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (UINT slot = 0; SUCCEEDED(hr) && slot < FCS_SLOT_COUNT; ++slot) {
        hr = g_forcedHostRing.device->CreateTexture2D(
            &desc, nullptr, &g_forcedHostRing.textures[slot]);
        if (FAILED(hr)) {
            stage = FCS_RESOURCE_STAGE_CREATE_TEXTURE;
            break;
        }
        hr = g_forcedHostRing.textures[slot]->QueryInterface(
            __uuidof(IDXGIKeyedMutex),
            reinterpret_cast<void**>(&g_forcedHostRing.mutexes[slot]));
        if (FAILED(hr)) {
            stage = FCS_RESOURCE_STAGE_KEYED_MUTEX;
            break;
        }
        if (requestedMode == FCS_SHARING_HOST_NT_NAME) {
            IDXGIResource1* resource = nullptr;
            hr = g_forcedHostRing.textures[slot]->QueryInterface(
                __uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource));
            if (FAILED(hr)) {
                stage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
                ReleaseCom(resource);
                break;
            }
            swprintf_s(names[slot], 96, L"Local\\FCS5.E2EHost.%lu.%I64x.%u",
                       static_cast<unsigned long>(GetCurrentProcessId()),
                       static_cast<unsigned long long>(metadata.resourceRequestId), slot);
            hr = resource->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                names[slot], &g_forcedHostRing.ntHandles[slot]);
            ReleaseCom(resource);
            if (SUCCEEDED(hr) && !g_forcedHostRing.ntHandles[slot]) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                handles[slot] = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(g_forcedHostRing.ntHandles[slot]));
            }
        } else {
            IDXGIResource* resource = nullptr;
            hr = g_forcedHostRing.textures[slot]->QueryInterface(
                __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
            if (FAILED(hr)) {
                stage = FCS_RESOURCE_STAGE_DXGI_RESOURCE;
                ReleaseCom(resource);
                break;
            }
            HANDLE sharedHandle = nullptr;
            hr = resource->GetSharedHandle(&sharedHandle);
            ReleaseCom(resource);
            if (SUCCEEDED(hr) && !sharedHandle) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                handles[slot] = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(sharedHandle));
            }
        }
        if (FAILED(hr)) stage = FCS_RESOURCE_STAGE_CREATE_HANDLE;
    }

    if (FAILED(hr)) {
        PublishForcedHostResponse(ipc, metadata.resourceRequestId, requestedMode,
                                  hr, stage, names, handles);
        DestroyForcedHostRing();
        return false;
    }
    g_forcedHostRing.requestId = metadata.resourceRequestId;
    g_forcedHostRing.sharingMode = requestedMode;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        g_forcedHostRing.publishedHandles[slot] = handles[slot];
    }
    PublishForcedHostResponse(ipc, metadata.resourceRequestId, requestedMode,
                              S_OK, FCS_RESOURCE_STAGE_NONE, names, handles);
    return true;
}

HRESULT OpenSharedTexture(ID3D11Device* device, ID3D11Device1* device1,
                          const StableMetadata& metadata, UINT slot,
                          ID3D11Texture2D** texture) {
    if (metadata.sharingMode == FCS_SHARING_NT_NAME ||
        metadata.sharingMode == FCS_SHARING_HOST_NT_NAME) {
        if (!device1) return E_NOINTERFACE;
        return device1->OpenSharedResourceByName(
            metadata.names[slot], DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture));
    }
    if (metadata.sharingMode == FCS_SHARING_LEGACY_HANDLE ||
        metadata.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE ||
        metadata.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE) {
        const HANDLE sharedHandle = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(metadata.handles[slot]));
        return device->OpenSharedResource(
            sharedHandle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture));
    }
    return E_INVALIDARG;
}

bool VerifyCapturedPixels(FcsIpcV1* ipc, const StableMetadata& metadata,
                          UINT expectedWidth, UINT expectedHeight,
                          const wchar_t* phase, uint32_t& redPixelCount) {
    if (metadata.format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        metadata.width != expectedWidth || metadata.height != expectedHeight) {
        wprintf(L"FAIL: unexpected %ls captured surface %ux%u format=%u\n", phase,
                metadata.width, metadata.height, static_cast<UINT>(metadata.format));
        return false;
    }

    const bool useCreatorResources =
        (metadata.sharingMode == FCS_SHARING_HOST_NT_NAME ||
         metadata.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE) &&
        metadata.resourceRequestId == g_forcedHostRing.requestId &&
        metadata.sharingMode == g_forcedHostRing.sharingMode;
    IDXGIAdapter1* adapter = useCreatorResources ? nullptr : FindAdapter(metadata.adapterLuid);
    ID3D11Device* device = nullptr;
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* shared[FCS_SLOT_COUNT]{};
    IDXGIKeyedMutex* mutexes[FCS_SLOT_COUNT]{};
    ID3D11Texture2D* staging = nullptr;
    bool verified = false;

    do {
        HRESULT hr = S_OK;
        if (useCreatorResources) {
            device = g_forcedHostRing.device;
            context = g_forcedHostRing.context;
            if (device) device->AddRef();
            if (context) context->AddRef();
            for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
                shared[slot] = g_forcedHostRing.textures[slot];
                mutexes[slot] = g_forcedHostRing.mutexes[slot];
                if (shared[slot]) shared[slot]->AddRef();
                if (mutexes[slot]) mutexes[slot]->AddRef();
            }
            if (!device || !context) hr = E_FAIL;
        } else {
            if (!adapter) {
                wprintf(L"FAIL: could not find producer GPU by LUID\n");
                break;
            }
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, &context);
            if (SUCCEEDED(hr)) {
                const HRESULT device1Hr = device->QueryInterface(
                    __uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
                if (metadata.sharingMode == FCS_SHARING_NT_NAME) hr = device1Hr;
            }
        }
        if (FAILED(hr)) {
            wprintf(L"FAIL: could not create D3D11.1 consumer (0x%08lx)\n",
                    static_cast<unsigned long>(hr));
            break;
        }

        if (!useCreatorResources) {
            for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
                hr = OpenSharedTexture(device, device1, metadata, slot, &shared[slot]);
                if (SUCCEEDED(hr)) hr = shared[slot]->QueryInterface(
                    __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutexes[slot]));
                if (FAILED(hr)) {
                    wprintf(L"FAIL: opening shared slot %u (0x%08lx)\n", slot,
                            static_cast<unsigned long>(hr));
                    break;
                }
            }
        }
        bool slotsReady = true;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            if (!shared[slot] || !mutexes[slot]) slotsReady = false;
        }
        if (!slotsReady) break;

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = metadata.width;
        stagingDesc.Height = metadata.height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = metadata.format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
        if (FAILED(hr)) {
            wprintf(L"FAIL: creating verification staging texture (0x%08lx)\n",
                    static_cast<unsigned long>(hr));
            break;
        }

        LONG newestSlot = -1;
        LONG64 newestSequence = 0;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = ipc->slotSequence[slot];
            if (sequence > newestSequence) {
                newestSequence = sequence;
                newestSlot = static_cast<LONG>(slot);
            }
        }
        if (newestSlot < 0 || mutexes[newestSlot]->AcquireSync(1, 2000) != S_OK) {
            wprintf(L"FAIL: no published keyed-mutex slot could be acquired\n");
            break;
        }
        context->CopyResource(staging, shared[newestSlot]);
        mutexes[newestSlot]->ReleaseSync(0);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            wprintf(L"FAIL: mapping captured frame (0x%08lx)\n",
                    static_cast<unsigned long>(hr));
            break;
        }

        auto pixelAt = [&](UINT x, UINT y) -> const uint8_t* {
            return static_cast<const uint8_t*>(mapped.pData) +
                   static_cast<size_t>(y) * mapped.RowPitch + x * 4;
        };
        const uint8_t* background = pixelAt(2, 2);
        const uint8_t* nativeHud = pixelAt(12, 12);
        const uint8_t* overlayLocation = pixelAt(52, 12);
        const bool blueScene = background[0] < 16 && background[1] < 16 && background[2] > 239;
        const bool greenHud = nativeHud[0] < 16 && nativeHud[1] > 239 && nativeHud[2] < 16;
        const bool overlayExcluded = overlayLocation[0] < 16 && overlayLocation[1] < 16 &&
                                     overlayLocation[2] > 239;
        redPixelCount = 0;
        for (UINT y = 0; y < metadata.height; ++y) {
            for (UINT x = 0; x < metadata.width; ++x) {
                const uint8_t* pixel = pixelAt(x, y);
                if (pixel[0] > 239 && pixel[1] < 16 && pixel[2] < 16) ++redPixelCount;
            }
        }
        wprintf(L"%ls samples: scene=(%u,%u,%u) HUD=(%u,%u,%u) overlay-site=(%u,%u,%u)\n",
                phase,
                background[0], background[1], background[2],
                nativeHud[0], nativeHud[1], nativeHud[2],
                overlayLocation[0], overlayLocation[1], overlayLocation[2]);
        context->Unmap(staging, 0);
        verified = blueScene && greenHud && overlayExcluded && redPixelCount == 0;
        if (!verified) wprintf(L"FAIL: clean-frame color assertions failed (red pixels=%u)\n",
                               redPixelCount);
    } while (false);

    ReleaseCom(staging);
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        ReleaseCom(mutexes[slot]);
        ReleaseCom(shared[slot]);
    }
    ReleaseCom(context);
    ReleaseCom(device1);
    ReleaseCom(device);
    ReleaseCom(adapter);
    return verified;
}

} // namespace fcs_test
