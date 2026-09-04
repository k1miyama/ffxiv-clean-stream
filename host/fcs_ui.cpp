#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fcs_controller.hpp"
#include "fcs_preview_window.hpp"
#include "fcs_ui.hpp"

namespace fcs::host {
namespace {

constexpr int IDC_START = 1001;
constexpr int IDC_PAUSE = 1002;
constexpr int IDC_FPS = 1003;
constexpr int IDC_STATUS = 1004;
constexpr int IDC_END = 1005;
constexpr int IDC_RESOLUTION = 1006;
constexpr int IDC_COPY_ERROR = 1007;
constexpr UINT_PTR STATUS_TIMER = 1;

HostController* ControllerForWindow(HWND window) {
    return reinterpret_cast<HostController*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

bool BindController(HWND window, LPARAM lParam) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    auto* controller = static_cast<HostController*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(controller));
    return controller != nullptr;
}

LRESULT CALLBACK ControlWindowProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    if (message == WM_NCCREATE) {
        if (!BindController(window, lParam)) return FALSE;
    }
    HostController* controller = ControllerForWindow(window);
    switch (message) {
    case WM_CREATE: {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const HINSTANCE instance =
            static_cast<HINSTANCE>(create->hInstance);
        HFONT font =
            static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND intro = CreateWindowExW(
            0, L"STATIC",
            L"1. Start FFXIV and make sure the MMOMinion GUI is visible.\r\n"
            L"2. Choose your settings, then click Start / Resume.\r\n"
            L"3. In Discord, share ‘FFXIV Clean Stream’ — not FFXIV.",
            WS_CHILD | WS_VISIBLE, 18, 16, 550, 64, window, nullptr,
            instance, nullptr);
        SendMessageW(intro, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND start = CreateWindowExW(
            0, L"BUTTON", L"Start / Resume",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 18, 90, 150, 34,
            window, reinterpret_cast<HMENU>(IDC_START), instance, nullptr);
        SendMessageW(start, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND pause = CreateWindowExW(
            0, L"BUTTON", L"Pause capture", WS_CHILD | WS_VISIBLE, 180, 90,
            130, 34, window, reinterpret_cast<HMENU>(IDC_PAUSE), instance,
            nullptr);
        SendMessageW(pause, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND end = CreateWindowExW(
            0, L"BUTTON", L"End stream", WS_CHILD | WS_VISIBLE, 322, 90,
            130, 34, window, reinterpret_cast<HMENU>(IDC_END), instance,
            nullptr);
        SendMessageW(end, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND copyError = CreateWindowExW(
            0, L"BUTTON", L"Copy error",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 464, 90, 108, 34, window,
            reinterpret_cast<HMENU>(IDC_COPY_ERROR), instance, nullptr);
        SendMessageW(copyError, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        HWND fpsLabel = CreateWindowExW(
            0, L"STATIC", L"Frame rate:", WS_CHILD | WS_VISIBLE, 18, 146,
            75, 22, window, nullptr, instance, nullptr);
        SendMessageW(fpsLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
        HWND fpsCombo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 96, 142, 185, 120,
            window, reinterpret_cast<HMENU>(IDC_FPS), instance, nullptr);
        SendMessageW(fpsCombo, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(fpsCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"30 FPS (recommended)"));
        SendMessageW(fpsCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"60 FPS"));
        SendMessageW(fpsCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"15 FPS (lightest)"));
        SendMessageW(fpsCombo, CB_SETCURSEL, 0, 0);
        HWND resolutionLabel = CreateWindowExW(
            0, L"STATIC", L"Resolution:", WS_CHILD | WS_VISIBLE, 304, 146,
            75, 22, window, nullptr, instance, nullptr);
        SendMessageW(resolutionLabel, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        HWND resolutionCombo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 382, 142, 190, 100,
            window, reinterpret_cast<HMENU>(IDC_RESOLUTION), instance,
            nullptr);
        SendMessageW(resolutionCombo, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(resolutionCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"1280 × 720 (16:9)"));
        SendMessageW(resolutionCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"1920 × 1080 (16:9)"));
        SendMessageW(resolutionCombo, CB_SETCURSEL, 0, 0);
        HWND statusLabel = CreateWindowExW(
            0, L"STATIC",
            L"Nothing is attached. This app will not touch FFXIV until you click Start / Resume.",
            WS_CHILD | WS_VISIBLE, 18, 190, 554, 170, window,
            reinterpret_cast<HMENU>(IDC_STATUS), instance, nullptr);
        SendMessageW(statusLabel, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        if (controller) {
            controller->BindControls(statusLabel, fpsCombo, copyError);
        }
        SetTimer(window, STATUS_TIMER, 16, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (!controller) return 0;
        if (LOWORD(wParam) == IDC_START) {
            controller->StartCapture();
        }
        if (LOWORD(wParam) == IDC_PAUSE) {
            controller->PauseCapture();
        }
        if (LOWORD(wParam) == IDC_END) {
            controller->EndCapture();
        }
        if (LOWORD(wParam) == IDC_COPY_ERROR &&
            HIWORD(wParam) == BN_CLICKED) {
            controller->CopyErrorToClipboard();
        }
        if (LOWORD(wParam) == IDC_RESOLUTION &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT selection = SendMessageW(
                reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0);
            controller->SetStreamResolution(
                selection == 1 ? StreamResolution::FullHd1080
                               : StreamResolution::Hd720);
        }
        return 0;
    case WM_TIMER:
        if (controller && wParam == STATUS_TIMER) controller->Tick();
        return 0;
    case WM_DESTROY:
        KillTimer(window, STATUS_TIMER);
        if (controller) controller->Shutdown();
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

bool CreateHostWindows(HINSTANCE instance, int showCommand,
                       HostController& controller) {
    WNDCLASSEXW controlClass{};
    controlClass.cbSize = sizeof(controlClass);
    controlClass.lpfnWndProc = ControlWindowProc;
    controlClass.hInstance = instance;
    controlClass.hCursor =
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    controlClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    controlClass.lpszClassName = L"FcsControlWindow";
    RegisterClassExW(&controlClass);

    HWND previewWindow = CreatePreviewWindow(
        instance, controller, kDefaultStreamResolution);
    controller.BindPreviewWindow(previewWindow);
    HWND controlWindow = CreateWindowExW(
        0, L"FcsControlWindow",
        L"FFXIV Clean Stream — Standalone Controls",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 610, 420, nullptr, nullptr, instance,
        &controller);
    if (!previewWindow || !controlWindow) {
        if (controlWindow) DestroyWindow(controlWindow);
        if (previewWindow) DestroyWindow(previewWindow);
        return false;
    }
    ShowWindow(controlWindow, showCommand);
    UpdateWindow(controlWindow);
    return true;
}

} // namespace fcs::host
