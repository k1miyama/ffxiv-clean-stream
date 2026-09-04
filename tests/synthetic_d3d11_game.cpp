#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "MinHook.h"
#include "e2e_test_protocol.h"

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

constexpr UINT kWidth = 96;
constexpr UINT kHeight = 64;
constexpr UINT kResizedWidth = 128;
constexpr UINT kResizedHeight = 80;
constexpr UINT kMarkerSize = 16;
constexpr UINT kHudX = 8;
constexpr UINT kHudY = 8;
constexpr UINT kOverlayX = 48;
constexpr UINT kOverlayY = 8;
constexpr UINT kLateOverlayX = 72;
constexpr size_t kSwapChain4Slots = 41;

FcsTestGameIpc* g_testIpc = nullptr;
PresentFn g_realPresent = nullptr;
PresentFn g_lateNextPresent = nullptr;
ID3D11Texture2D* g_redMarker = nullptr;
ID3D11DeviceContext* g_context = nullptr;
void** g_lateOverlayVtable = nullptr;

template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void SetTestError(const wchar_t* message) {
    if (!g_testIpc) return;
    lstrcpynW(g_testIpc->message, message,
              static_cast<int>(sizeof(g_testIpc->message) / sizeof(wchar_t)));
    InterlockedExchange(&g_testIpc->state, FCS_TEST_GAME_ERROR);
}

HRESULT STDMETHODCALLTYPE MockOverlayPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    // This red marker deliberately runs downstream of the capture helper. If
    // the helper copies at the right point, the marker is visible in the real
    // swapchain but absent from the shared clean texture.
    ID3D11Texture2D* backBuffer = nullptr;
    if (g_context && g_redMarker && SUCCEEDED(swap->GetBuffer(
            0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        g_context->CopySubresourceRegion(backBuffer, 0, kOverlayX, kOverlayY, 0,
                                         g_redMarker, 0, nullptr);
        backBuffer->Release();
        if (g_testIpc) InterlockedIncrement64(&g_testIpc->overlayRuns);
    }
    return g_realPresent ? g_realPresent(swap, syncInterval, flags) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE LateOverlayPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    // Unlike the initial code hook above, this overlay is installed by
    // replacing the live swap-chain vtable after the production proxy exists.
    // Increment unconditionally so the regression test measures calls rather
    // than successful marker uploads.
    if (g_testIpc) InterlockedIncrement64(&g_testIpc->lateOverlayRuns);
    ID3D11Texture2D* backBuffer = nullptr;
    if (g_context && g_redMarker && SUCCEEDED(swap->GetBuffer(
            0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        g_context->CopySubresourceRegion(backBuffer, 0, kLateOverlayX, kOverlayY, 0,
                                         g_redMarker, 0, nullptr);
        backBuffer->Release();
    }
    return g_lateNextPresent ? g_lateNextPresent(swap, syncInterval, flags) : E_FAIL;
}

bool EnsureLateOverlayInstalled(IDXGISwapChain* swap) {
    if (!swap) return false;
    void** current = *reinterpret_cast<void***>(swap);
    if (!current) return false;
    if (current == g_lateOverlayVtable &&
        current[8] == reinterpret_cast<void*>(&LateOverlayPresent)) {
        return true;
    }

    if (!g_lateOverlayVtable) {
        g_lateOverlayVtable = static_cast<void**>(VirtualAlloc(
            nullptr, kSwapChain4Slots * sizeof(void*),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_lateOverlayVtable) return false;
    }
    memcpy(g_lateOverlayVtable, current, kSwapChain4Slots * sizeof(void*));
    g_lateNextPresent = reinterpret_cast<PresentFn>(current[8]);
    g_lateOverlayVtable[8] = reinterpret_cast<void*>(&LateOverlayPresent);
    MemoryBarrier();
    void* previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(swap), g_lateOverlayVtable, current);
    if (previous != current) return false;
    if (g_testIpc) InterlockedIncrement(&g_testIpc->lateOverlayInstalls);
    return true;
}

LRESULT CALLBACK HiddenWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ID3D11Texture2D* CreateSolidMarker(ID3D11Device* device, uint32_t rgba) {
    uint32_t pixels[kMarkerSize * kMarkerSize];
    for (uint32_t& pixel : pixels) pixel = rgba;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kMarkerSize;
    desc.Height = kMarkerSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels;
    initial.SysMemPitch = kMarkerSize * sizeof(uint32_t);

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &texture))) return nullptr;
    return texture;
}

} // namespace

int main() {
    const DWORD pid = GetCurrentProcessId();
    wchar_t objectName[96]{};
    FcsTestGameMappingName(objectName, pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(FcsTestGameIpc), objectName);
    if (!mapping) return 10;
    g_testIpc = static_cast<FcsTestGameIpc*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FcsTestGameIpc)));
    if (!g_testIpc) {
        CloseHandle(mapping);
        return 11;
    }
    ZeroMemory(g_testIpc, sizeof(*g_testIpc));
    g_testIpc->magic = FCS_TEST_MAGIC;
    g_testIpc->bytes = sizeof(*g_testIpc);

    FcsTestGameReadyEventName(objectName, pid);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, objectName);
    if (!readyEvent) {
        SetTestError(L"Could not create the synthetic-game ready event.");
        return 12;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"FcsHiddenSyntheticD3D11Game";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = HiddenWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass)) {
        SetTestError(L"Could not register the hidden synthetic window.");
        SetEvent(readyEvent);
        return 13;
    }

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, className,
                                  L"FCS Hidden Synthetic Game", WS_POPUP,
                                  0, 0, kWidth, kHeight, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) {
        SetTestError(L"Could not create the hidden synthetic window.");
        SetEvent(readyEvent);
        return 14;
    }
    // Intentionally never call ShowWindow: the test has no visible UI.

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = kWidth;
    swapDesc.BufferDesc.Height = kHeight;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 1;
    swapDesc.OutputWindow = window;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* backBufferRtv = nullptr;
    ID3D11Texture2D* greenMarker = nullptr;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &swapDesc, &swap, &device,
        &featureLevel, &g_context);
    if (FAILED(hr)) {
        SetTestError(L"Could not create the synthetic Direct3D 11 device.");
        SetEvent(readyEvent);
        goto cleanup;
    }

    hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&backBuffer));
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(backBuffer, nullptr, &backBufferRtv);
    greenMarker = CreateSolidMarker(device, 0xFF00FF00u); // R=0, G=255, B=0, A=255
    g_redMarker = CreateSolidMarker(device, 0xFF0000FFu); // R=255, G=0, B=0, A=255
    if (FAILED(hr) || !backBufferRtv || !greenMarker || !g_redMarker) {
        SetTestError(L"Could not create the synthetic render targets.");
        SetEvent(readyEvent);
        goto cleanup;
    }

    {
        void* presentTarget = (*reinterpret_cast<void***>(swap))[8];
        MH_STATUS hookStatus = MH_Initialize();
        if (hookStatus != MH_OK && hookStatus != MH_ERROR_ALREADY_INITIALIZED) {
            SetTestError(L"The mock overlay could not initialize MinHook.");
            SetEvent(readyEvent);
            goto cleanup;
        }
        hookStatus = MH_CreateHook(presentTarget,
                                   reinterpret_cast<void*>(&MockOverlayPresent),
                                   reinterpret_cast<void**>(&g_realPresent));
        if (hookStatus != MH_OK) {
            SetTestError(L"The mock overlay could not hook Present.");
            SetEvent(readyEvent);
            goto cleanup;
        }
        hookStatus = MH_EnableHook(presentTarget);
        if (hookStatus != MH_OK && hookStatus != MH_ERROR_ENABLED) {
            SetTestError(L"The mock overlay could not enable its Present hook.");
            SetEvent(readyEvent);
            goto cleanup;
        }
    }

    InterlockedExchange64(&g_testIpc->targetHwnd,
                          static_cast<LONG64>(reinterpret_cast<uintptr_t>(window)));
    lstrcpynW(g_testIpc->message, L"Synthetic game and downstream overlay are ready.",
              static_cast<int>(sizeof(g_testIpc->message) / sizeof(wchar_t)));
    InterlockedExchange(&g_testIpc->state, FCS_TEST_GAME_READY);
    SetEvent(readyEvent);

    {
        LARGE_INTEGER frequency{}, start{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        bool running = true;
        bool lateOverlayActive = false;
        while (running && !g_testIpc->stopRequested) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) running = false;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;

            if (InterlockedExchange(&g_testIpc->resizeRequested, 0) != 0) {
                ReleaseCom(backBufferRtv);
                ReleaseCom(backBuffer);
                hr = swap->ResizeBuffers(1, kResizedWidth, kResizedHeight,
                                         DXGI_FORMAT_R8G8B8A8_UNORM, 0);
                if (SUCCEEDED(hr)) {
                    hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void**>(&backBuffer));
                }
                if (SUCCEEDED(hr)) {
                    hr = device->CreateRenderTargetView(backBuffer, nullptr, &backBufferRtv);
                }
                if (FAILED(hr) || !backBuffer || !backBufferRtv) {
                    SetTestError(L"Synthetic ResizeBuffers failed.");
                    running = false;
                    break;
                }
                SetWindowPos(window, nullptr, 0, 0, kResizedWidth, kResizedHeight,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                InterlockedExchange(&g_testIpc->resizedWidth,
                                    static_cast<LONG>(kResizedWidth));
                InterlockedExchange(&g_testIpc->resizedHeight,
                                    static_cast<LONG>(kResizedHeight));
                InterlockedIncrement(&g_testIpc->resizeCompleted);
            }

            if (g_testIpc->lateRehookRequested) {
                if (!EnsureLateOverlayInstalled(swap)) {
                    SetTestError(L"Synthetic late-overlay vtable install failed.");
                    running = false;
                    break;
                }
                lateOverlayActive = true;
                InterlockedExchange(&g_testIpc->lateRehookCompleted, 1);
            }

            const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            g_context->ClearRenderTargetView(backBufferRtv, blue);
            g_context->CopySubresourceRegion(backBuffer, 0, kHudX, kHudY, 0,
                                             greenMarker, 0, nullptr);
            const LONG64 lateRunsBefore = g_testIpc->lateOverlayRuns;
            swap->Present(0, 0);
            if (lateOverlayActive) {
                const LONG64 calls = g_testIpc->lateOverlayRuns - lateRunsBefore;
                InterlockedIncrement64(&g_testIpc->latePresentFrames);
                if (calls == 0) {
                    InterlockedIncrement64(&g_testIpc->lateOverlayMissingFrames);
                } else if (calls > 1) {
                    InterlockedIncrement64(&g_testIpc->lateOverlayDuplicateFrames);
                }
                LONG observed = g_testIpc->lateOverlayMaxCallsPerPresent;
                while (calls > observed &&
                       InterlockedCompareExchange(
                           &g_testIpc->lateOverlayMaxCallsPerPresent,
                           static_cast<LONG>(calls), observed) != observed) {
                    observed = g_testIpc->lateOverlayMaxCallsPerPresent;
                }
            }
            InterlockedIncrement64(&g_testIpc->gameFrames);

            QueryPerformanceCounter(&now);
            if (now.QuadPart - start.QuadPart > frequency.QuadPart * 20) break;
            Sleep(2);
        }
    }

cleanup:
    // Do not detach the mock hook here: the real capture helper may be chained
    // through its trampoline. Process exit safely tears both test hooks down.
    ReleaseCom(g_redMarker);
    ReleaseCom(greenMarker);
    ReleaseCom(backBufferRtv);
    ReleaseCom(backBuffer);
    ReleaseCom(g_context);
    ReleaseCom(device);
    ReleaseCom(swap);
    if (g_lateOverlayVtable) {
        VirtualFree(g_lateOverlayVtable, 0, MEM_RELEASE);
        g_lateOverlayVtable = nullptr;
    }
    if (window) DestroyWindow(window);
    UnregisterClassW(className, instance);
    CloseHandle(readyEvent);
    UnmapViewOfFile(g_testIpc);
    g_testIpc = nullptr;
    CloseHandle(mapping);
    return 0;
}
