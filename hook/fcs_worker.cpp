#include "fcs_internal.hpp"
#include "MinHook.h"

namespace fcs::hook {
namespace {

LRESULT CALLBACK DummyWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

bool FindPresentAddresses() {
    const wchar_t* className = L"FcsDummyDxgiWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWindowProc;
    wc.hInstance = g_processState.module;
    wc.lpszClassName = className;
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, className, L"", WS_POPUP,
                                  0, 0, 2, 2, nullptr, nullptr, g_processState.module, nullptr);
    if (!window) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 2;
    desc.BufferDesc.Height = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &swap, &device, &level, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &desc, &swap, &device, &level, &context);
    }
    if (SUCCEEDED(hr) && swap) {
        g_swapChainState.presentTarget = (*reinterpret_cast<void***>(swap))[8];
        IDXGISwapChain1* swap1 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1),
                                           reinterpret_cast<void**>(&swap1)))) {
            g_swapChainState.present1Target = (*reinterpret_cast<void***>(swap1))[22];
            swap1->Release();
        }
    }
    ReleaseCom(context);
    ReleaseCom(device);
    ReleaseCom(swap);
    DestroyWindow(window);
    UnregisterClassW(className, g_processState.module);
    return g_swapChainState.presentTarget != nullptr;
}

bool OpenIpc() {
    wchar_t name[96]{};
    const DWORD pid = GetCurrentProcessId();
    FcsMappingName(name, pid);
    for (int attempt = 0; attempt < 100; ++attempt) {
        g_ipcState.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (g_ipcState.mapping) break;
        Sleep(100);
    }
    if (!g_ipcState.mapping) return false;
    g_ipcState.ipc = static_cast<FcsIpcV1*>(MapViewOfFile(
        g_ipcState.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FcsIpcV1)));
    if (!g_ipcState.ipc || g_ipcState.ipc->magic != FCS_MAGIC ||
        g_ipcState.ipc->version != FCS_VERSION || g_ipcState.ipc->bytes != sizeof(FcsIpcV1)) {
        if (g_ipcState.ipc) UnmapViewOfFile(g_ipcState.ipc);
        CloseHandle(g_ipcState.mapping);
        g_ipcState.ipc = nullptr;
        g_ipcState.mapping = nullptr;
        return false;
    }

    FcsReadyEventName(name, pid);
    g_ipcState.readyEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    FcsFrameEventName(name, pid);
    g_ipcState.frameEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (!g_ipcState.readyEvent || !g_ipcState.frameEvent) {
        if (g_ipcState.frameEvent) CloseHandle(g_ipcState.frameEvent);
        if (g_ipcState.readyEvent) CloseHandle(g_ipcState.readyEvent);
        UnmapViewOfFile(g_ipcState.ipc);
        CloseHandle(g_ipcState.mapping);
        g_ipcState.frameEvent = nullptr;
        g_ipcState.readyEvent = nullptr;
        g_ipcState.ipc = nullptr;
        g_ipcState.mapping = nullptr;
        return false;
    }
    return true;
}

} // namespace

DWORD WINAPI HookWorker(void*) {
    while (!OpenIpc()) Sleep(1000);
    QueryPerformanceFrequency(&g_captureState.qpcFrequency);
    g_ipcState.ipc->qpcFrequency = g_captureState.qpcFrequency.QuadPart;
    g_ipcState.ipc->producerPid = GetCurrentProcessId();
    SetMessage(L"Injected. Waiting for FFXIV's main Direct3D swap chain...");
    InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_INJECTED);
    SetEvent(g_ipcState.readyEvent);

    MH_STATUS status = MH_Initialize();
    while (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        SetError(status, L"The Direct3D hook engine could not initialize; retrying.");
        Sleep(2000);
        status = MH_Initialize();
    }

    while (true) {
        g_swapChainState.presentTarget = nullptr;
        g_swapChainState.present1Target = nullptr;
        if (!FindPresentAddresses()) {
            SetError(E_FAIL, L"Could not locate Direct3D 11 Present; retrying.");
            Sleep(2000);
            continue;
        }
        status = MH_CreateHook(g_swapChainState.presentTarget, reinterpret_cast<void*>(&BootstrapPresent),
                               reinterpret_cast<void**>(&g_swapChainState.bootstrapOriginal));
        if (status == MH_ERROR_ALREADY_CREATED && g_swapChainState.bootstrapOriginal) status = MH_OK;
        if (status != MH_OK) {
            SetError(status, L"Present is hooked incompatibly; retrying.");
            Sleep(2000);
            continue;
        }
        status = MH_EnableHook(g_swapChainState.presentTarget);
        if (status == MH_OK || status == MH_ERROR_ENABLED) break;
        SetError(status, L"Could not enable the Direct3D Present hook; retrying.");
        MH_RemoveHook(g_swapChainState.presentTarget);
        g_swapChainState.bootstrapOriginal = nullptr;
        Sleep(2000);
    }

    if (g_swapChainState.present1Target && g_swapChainState.present1Target != g_swapChainState.presentTarget) {
        status = MH_CreateHook(g_swapChainState.present1Target, reinterpret_cast<void*>(&BootstrapPresent1),
                               reinterpret_cast<void**>(&g_swapChainState.bootstrapOriginal1));
        if (status == MH_OK) status = MH_EnableHook(g_swapChainState.present1Target);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            MH_RemoveHook(g_swapChainState.present1Target);
            g_swapChainState.bootstrapOriginal1 = nullptr;
        }
    }
    // The game can Present on another thread before hook installation returns.
    // Preserve any HOOKED/WAITING/STREAMING transition made by that thread.
    if (g_ipcState.ipc->state == FCS_STATE_INJECTED) {
        SetMessage(g_swapChainState.bootstrapOriginal1
            ? L"Injected. Waiting for FFXIV's main Direct3D swap chain..."
            : L"Injected with base Present compatibility. Waiting for FFXIV...");
    }

    HANDLE controllerWatch = nullptr;
    DWORD watchedControllerPid = 0;
    while (true) {
        Sleep(250);
        if (!g_ipcState.ipc) break;
        const DWORD controllerPid = g_ipcState.ipc->controllerPid;
        if (controllerPid != watchedControllerPid ||
            (controllerPid != 0 && !controllerWatch)) {
            if (controllerWatch) CloseHandle(controllerWatch);
            controllerWatch = nullptr;
            watchedControllerPid = 0;
            if (controllerPid != 0) {
                controllerWatch = OpenProcess(SYNCHRONIZE, FALSE, controllerPid);
                if (controllerWatch) watchedControllerPid = controllerPid;
            }
        }
        if (controllerWatch && WaitForSingleObject(controllerWatch, 0) == WAIT_OBJECT_0 &&
            g_ipcState.ipc->controllerPid == watchedControllerPid) {
            InterlockedExchange(&g_ipcState.ipc->stopRequested, 1);
        }
        if (g_swapChainState.hookOrderLost) {
            InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_HOOK_ORDER_LOST);
        } else if (g_ipcState.ipc->stopRequested) {
            InterlockedExchange(&g_ipcState.ipc->state, FCS_STATE_PAUSED);
        }
    }
    if (controllerWatch) CloseHandle(controllerWatch);
    return 0;
}

} // namespace fcs::hook
