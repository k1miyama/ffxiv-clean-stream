#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

#include "../common/fcs_formats.hpp"

template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

constexpr DXGI_FORMAT kGuaranteedSharedFormats[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_B8G8R8X8_UNORM,
    DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
};

static_assert(FcsTransportFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
static_assert(FcsPreviewFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
              DXGI_FORMAT_R8G8B8A8_UNORM);
static_assert(FcsTransportFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS) ==
              DXGI_FORMAT_B8G8R8A8_UNORM);
static_assert(FcsTransportFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS) ==
              DXGI_FORMAT_R10G10B10A2_UNORM);
static_assert(FcsTransportFormat(DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_UNKNOWN);

HRESULT ProbeSharedFormat(ID3D11Device* producer, ID3D11Device1* consumer,
                          DXGI_FORMAT format) {
    ID3D11Texture2D* producerTexture = nullptr;
    ID3D11Texture2D* consumerTexture = nullptr;
    IDXGIResource1* resource = nullptr;
    IDXGIKeyedMutex* producerMutex = nullptr;
    IDXGIKeyedMutex* consumerMutex = nullptr;
    HANDLE sharedHandle = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                     D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = producer->CreateTexture2D(&desc, nullptr, &producerTexture);
    if (SUCCEEDED(hr)) {
        hr = producerTexture->QueryInterface(
            __uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource));
    }
    if (SUCCEEDED(hr)) {
        hr = producerTexture->QueryInterface(
            __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&producerMutex));
    }

    wchar_t resourceName[96]{};
    LARGE_INTEGER nonce{};
    QueryPerformanceCounter(&nonce);
    swprintf_s(resourceName, sizeof(resourceName) / sizeof(resourceName[0]),
               L"Local\\FCS5.SelfTest.%lu.%I64x.%u",
               static_cast<unsigned long>(GetCurrentProcessId()),
               static_cast<unsigned long long>(nonce.QuadPart),
               static_cast<unsigned int>(format));
    if (SUCCEEDED(hr)) {
        hr = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            resourceName, &sharedHandle);
    }
    if (SUCCEEDED(hr)) {
        hr = consumer->OpenSharedResourceByName(
            resourceName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&consumerTexture));
    }
    if (SUCCEEDED(hr)) {
        hr = consumerTexture->QueryInterface(
            __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&consumerMutex));
    }

    if (sharedHandle) CloseHandle(sharedHandle);
    ReleaseCom(consumerMutex);
    ReleaseCom(producerMutex);
    ReleaseCom(resource);
    ReleaseCom(consumerTexture);
    ReleaseCom(producerTexture);
    return hr;
}

HRESULT ProbeLegacySharedFormat(ID3D11Device* producer, ID3D11Device* consumer,
                                DXGI_FORMAT format) {
    ID3D11Texture2D* producerTexture = nullptr;
    ID3D11Texture2D* consumerTexture = nullptr;
    IDXGIResource* resource = nullptr;
    IDXGIKeyedMutex* producerMutex = nullptr;
    IDXGIKeyedMutex* consumerMutex = nullptr;
    HANDLE sharedHandle = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = producer->CreateTexture2D(&desc, nullptr, &producerTexture);
    if (SUCCEEDED(hr)) {
        hr = producerTexture->QueryInterface(
            __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
    }
    if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&sharedHandle);
    if (SUCCEEDED(hr) && !sharedHandle) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = consumer->OpenSharedResource(
            sharedHandle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&consumerTexture));
    }
    if (SUCCEEDED(hr)) {
        hr = producerTexture->QueryInterface(
            __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&producerMutex));
    }
    if (SUCCEEDED(hr)) {
        hr = consumerTexture->QueryInterface(
            __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&consumerMutex));
    }

    // GetSharedHandle returns an opaque DXGI value, not a CloseHandle-able NT handle.
    ReleaseCom(consumerMutex);
    ReleaseCom(producerMutex);
    ReleaseCom(resource);
    ReleaseCom(consumerTexture);
    ReleaseCom(producerTexture);
    return hr;
}

HRESULT ProbePlainLegacySharedFormat(ID3D11Device* producer,
                                     ID3D11Device* consumer,
                                     DXGI_FORMAT format) {
    ID3D11Texture2D* producerTexture = nullptr;
    ID3D11Texture2D* consumerTexture = nullptr;
    IDXGIResource* resource = nullptr;
    HANDLE sharedHandle = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = producer->CreateTexture2D(&desc, nullptr, &producerTexture);
    if (SUCCEEDED(hr)) {
        hr = producerTexture->QueryInterface(
            __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
    }
    if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&sharedHandle);
    if (SUCCEEDED(hr) && !sharedHandle) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = consumer->OpenSharedResource(
            sharedHandle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&consumerTexture));
    }
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC opened{};
        consumerTexture->GetDesc(&opened);
        if ((opened.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0 ||
            (opened.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0 ||
            opened.Width != desc.Width || opened.Height != desc.Height ||
            opened.Format != desc.Format) {
            hr = E_INVALIDARG;
        }
    }

    // GetSharedHandle returns an opaque DXGI value, not an NT handle.
    ReleaseCom(resource);
    ReleaseCom(consumerTexture);
    ReleaseCom(producerTexture);
    return hr;
}

int main() {
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D11Device* producer = nullptr;
    ID3D11Device1* producer1 = nullptr;
    ID3D11Device1* consumer1 = nullptr;
    ID3D11Device* consumer = nullptr;
    ID3D11DeviceContext* producerContext = nullptr;
    ID3D11DeviceContext* consumerContext = nullptr;
    ID3D11Texture2D* source = nullptr;
    ID3D11RenderTargetView* sourceRtv = nullptr;
    ID3D11Texture2D* sharedProducer = nullptr;
    ID3D11Texture2D* sharedConsumer = nullptr;
    IDXGIKeyedMutex* producerMutex = nullptr;
    IDXGIKeyedMutex* consumerMutex = nullptr;
    ID3D11Texture2D* staging = nullptr;
    IDXGIResource1* sharedResource = nullptr;
    ID3D11Texture2D* benchmarkTexture = nullptr;
    HANDLE sharedHandle = nullptr;
    int result = 1;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    D3D11_TEXTURE2D_DESC sharedDesc{};
    D3D11_TEXTURE2D_DESC stagingDesc{};
    D3D11_TEXTURE2D_DESC benchmarkDesc{};
    const float color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const uint8_t* pixel = nullptr;
    bool colorOk = false;
    LARGE_INTEGER frequency{}, start{}, end{};
    double averageMicros = 0.0;

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory->EnumAdapters1(0, &adapter) != S_OK) {
        printf("FAIL: no DXGI adapter (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &producer, nullptr, &producerContext);
    if (FAILED(hr)) {
        printf("FAIL: producer device (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }
    hr = producer->QueryInterface(
        __uuidof(ID3D11Device1), reinterpret_cast<void**>(&producer1));
    if (FAILED(hr)) {
        printf("FAIL: producer D3D11.1 device (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &consumer, nullptr, &consumerContext);
    if (SUCCEEDED(hr)) {
        hr = consumer->QueryInterface(
            __uuidof(ID3D11Device1), reinterpret_cast<void**>(&consumer1));
    }
    if (FAILED(hr)) {
        printf("FAIL: consumer D3D11.1 device (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }

    for (DXGI_FORMAT format : kGuaranteedSharedFormats) {
        hr = ProbeSharedFormat(producer, consumer1, format);
        if (FAILED(hr)) {
            printf("FAIL: guaranteed shared format %u (0x%08lx)\n",
                   static_cast<unsigned int>(format),
                   static_cast<unsigned long>(hr));
            goto cleanup;
        }
    }
    printf("Shared-format matrix: %zu documented formats passed\n",
           sizeof(kGuaranteedSharedFormats) / sizeof(kGuaranteedSharedFormats[0]));

    // Reverse the real capture ownership: the clean-preview device creates
    // the keyed resource and the game-side device only opens it.
    hr = ProbeSharedFormat(consumer, producer1, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (FAILED(hr)) {
        printf("FAIL: host-owned reverse sharing (0x%08lx)\n",
               static_cast<unsigned long>(hr));
        goto cleanup;
    }
    printf("Host-owned reverse keyed sharing: passed\n");

    hr = ProbeLegacySharedFormat(consumer, producer, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (FAILED(hr)) {
        printf("FAIL: host-owned reverse legacy sharing (0x%08lx)\n",
               static_cast<unsigned long>(hr));
        goto cleanup;
    }
    printf("Host-owned reverse legacy keyed sharing: passed\n");

    hr = ProbeLegacySharedFormat(producer, consumer, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (FAILED(hr)) {
        printf("FAIL: legacy keyed sharing fallback (0x%08lx)\n",
               static_cast<unsigned long>(hr));
        goto cleanup;
    }
    printf("Legacy keyed-sharing fallback: passed\n");

    hr = ProbePlainLegacySharedFormat(producer, consumer,
                                      DXGI_FORMAT_R8G8B8A8_UNORM);
    if (FAILED(hr)) {
        printf("FAIL: plain legacy sharing fallback (0x%08lx)\n",
               static_cast<unsigned long>(hr));
        goto cleanup;
    }
    printf("Plain legacy sharing fallback: passed\n");

    sourceDesc.Width = 64;
    sourceDesc.Height = 64;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    sourceDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = producer->CreateTexture2D(&sourceDesc, nullptr, &source);
    if (SUCCEEDED(hr)) hr = producer->CreateRenderTargetView(source, nullptr, &sourceRtv);
    if (FAILED(hr)) {
        printf("FAIL: source texture (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }

    sharedDesc = sourceDesc;
    sharedDesc.Format = FcsTransportFormat(sourceDesc.Format);
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                           D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    hr = producer->CreateTexture2D(&sharedDesc, nullptr, &sharedProducer);
    if (SUCCEEDED(hr)) hr = sharedProducer->QueryInterface(
        __uuidof(IDXGIResource1), reinterpret_cast<void**>(&sharedResource));
    if (SUCCEEDED(hr)) hr = sharedResource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &sharedHandle);
    if (SUCCEEDED(hr)) hr = consumer1->OpenSharedResource1(
        sharedHandle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sharedConsumer));
    if (sharedHandle) {
        CloseHandle(sharedHandle);
        sharedHandle = nullptr;
    }
    if (SUCCEEDED(hr)) hr = sharedProducer->QueryInterface(
        __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&producerMutex));
    if (SUCCEEDED(hr)) hr = sharedConsumer->QueryInterface(
        __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&consumerMutex));
    if (FAILED(hr)) {
        printf("FAIL: NT-handle keyed sharing (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }

    stagingDesc = sharedDesc;
    stagingDesc.Format = FcsPreviewFormat(sharedDesc.Format);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    hr = consumer->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        printf("FAIL: staging texture (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }

    producerContext->ClearRenderTargetView(sourceRtv, color);
    hr = producerMutex->AcquireSync(0, 0);
    if (hr == S_OK) {
        producerContext->CopyResource(sharedProducer, source);
        hr = producerMutex->ReleaseSync(1);
    }
    if (hr == S_OK) hr = consumerMutex->AcquireSync(1, 2000);
    if (hr == S_OK) {
        consumerContext->CopyResource(staging, sharedConsumer);
        hr = consumerMutex->ReleaseSync(0);
    }
    if (hr != S_OK) {
        printf("FAIL: nonblocking GPU handoff (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }

    hr = consumerContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        printf("FAIL: readback verification (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }
    pixel = static_cast<const uint8_t*>(mapped.pData);
    colorOk = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255;
    printf("Pixel: R=%u G=%u B=%u A=%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    consumerContext->Unmap(staging, 0);
    if (!colorOk) {
        printf("FAIL: shared image contents did not match\n");
        goto cleanup;
    }

    QueryPerformanceFrequency(&frequency);
    benchmarkDesc = sharedDesc;
    benchmarkDesc.MiscFlags = 0;
    benchmarkDesc.BindFlags = 0;
    hr = producer->CreateTexture2D(&benchmarkDesc, nullptr, &benchmarkTexture);
    if (FAILED(hr)) {
        printf("FAIL: benchmark texture (0x%08lx)\n", static_cast<unsigned long>(hr));
        goto cleanup;
    }
    QueryPerformanceCounter(&start);
    for (int i = 0; i < 5000; ++i) producerContext->CopyResource(benchmarkTexture, source);
    QueryPerformanceCounter(&end);
    averageMicros = static_cast<double>(end.QuadPart - start.QuadPart) *
                    1000000.0 / static_cast<double>(frequency.QuadPart) / 5000.0;
    printf("Average CopyResource submission: %.3f microseconds (no Flush)\n", averageMicros);
    printf("PASS: D3D11 keyed and plain legacy GPU sharing work on this machine\n");
    result = 0;

cleanup:
    if (sharedHandle) CloseHandle(sharedHandle);
    ReleaseCom(benchmarkTexture);
    ReleaseCom(sharedResource);
    ReleaseCom(staging);
    ReleaseCom(consumerMutex);
    ReleaseCom(producerMutex);
    ReleaseCom(sharedConsumer);
    ReleaseCom(sharedProducer);
    ReleaseCom(sourceRtv);
    ReleaseCom(source);
    ReleaseCom(consumerContext);
    ReleaseCom(producerContext);
    ReleaseCom(consumer1);
    ReleaseCom(consumer);
    ReleaseCom(producer1);
    ReleaseCom(producer);
    ReleaseCom(adapter);
    ReleaseCom(factory);
    return result;
}
