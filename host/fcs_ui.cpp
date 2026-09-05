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
constexpr int IDC_LANGUAGE = 1008;
constexpr int IDC_INTRO = 1009;
constexpr int IDC_FPS_LABEL = 1010;
constexpr int IDC_RESOLUTION_LABEL = 1011;
constexpr UINT_PTR STATUS_TIMER = 1;

void ApplyLanguage(HWND window, HostController& controller) {
    const UiLanguage language = controller.Language();
    SetWindowTextW(window, UiText(language, kControlTitle));
    const struct { int id; const wchar_t* text; } labels[] = {
        {IDC_INTRO, kIntroText}, {IDC_START, L"Start / Resume"},
        {IDC_PAUSE, L"Pause capture"}, {IDC_END, L"End stream"},
        {IDC_FPS_LABEL, L"Frame rate:"},
        {IDC_RESOLUTION_LABEL, L"Resolution:"},
    };
    for (const auto& label : labels) {
        SetDlgItemTextW(window, label.id, UiText(language, label.text));
    }
    HWND fps = GetDlgItem(window, IDC_FPS);
    const LRESULT selection = SendMessageW(fps, CB_GETCURSEL, 0, 0);
    SendMessageW(fps, CB_RESETCONTENT, 0, 0);
    const wchar_t* options[] = {
        L"30 FPS (recommended)", L"60 FPS", L"15 FPS (lightest)"};
    for (const wchar_t* option : options) {
        SendMessageW(fps, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UiText(language, option)));
    }
    SendMessageW(fps, CB_SETCURSEL, selection == CB_ERR ? 0 : selection, 0);
    SendDlgItemMessageW(window, IDC_LANGUAGE, CB_SETCURSEL,
                        language == UiLanguage::SimplifiedChinese ? 1 : 0, 0);
}

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
        // YaHei UI includes Simplified Chinese glyphs and readable Latin text.
        // Windows supplies a fallback if that optional font is unavailable.
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (font) SetPropW(window, L"FcsUiFont", font);
        else font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND languageLabel = CreateWindowExW(
            0, L"STATIC", L"Language / 语言", WS_CHILD | WS_VISIBLE,
            18, 20, 125, 24, window, nullptr, instance, nullptr);
        SendMessageW(languageLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND languageCombo = CreateWindowExW(
            0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            148, 16, 190, 100, window, reinterpret_cast<HMENU>(IDC_LANGUAGE),
            instance, nullptr);
        SendMessageW(languageCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"简体中文"));
        HWND intro = CreateWindowExW(
            0, L"STATIC", kIntroText,
            WS_CHILD | WS_VISIBLE, 18, 58, 644, 72, window, reinterpret_cast<HMENU>(IDC_INTRO),
            instance, nullptr);
        SendMessageW(intro, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND start = CreateWindowExW(
            0, L"BUTTON", L"Start / Resume",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 18, 140, 150, 34,
            window, reinterpret_cast<HMENU>(IDC_START), instance, nullptr);
        SendMessageW(start, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND pause = CreateWindowExW(
            0, L"BUTTON", L"Pause capture", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 180, 140,
            130, 34, window, reinterpret_cast<HMENU>(IDC_PAUSE), instance,
            nullptr);
        SendMessageW(pause, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND end = CreateWindowExW(
            0, L"BUTTON", L"End stream", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 322, 140,
            130, 34, window, reinterpret_cast<HMENU>(IDC_END), instance,
            nullptr);
        SendMessageW(end, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HWND copyError = CreateWindowExW(
            0, L"BUTTON", L"Copy error",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED, 464, 140, 198, 34, window,
            reinterpret_cast<HMENU>(IDC_COPY_ERROR), instance, nullptr);
        SendMessageW(copyError, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        HWND fpsLabel = CreateWindowExW(
            0, L"STATIC", L"Frame rate:", WS_CHILD | WS_VISIBLE, 18, 196,
            98, 24, window, reinterpret_cast<HMENU>(IDC_FPS_LABEL), instance, nullptr);
        SendMessageW(fpsLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
        HWND fpsCombo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 120, 192, 214, 120,
            window, reinterpret_cast<HMENU>(IDC_FPS), instance, nullptr);
        SendMessageW(fpsCombo, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        HWND resolutionLabel = CreateWindowExW(
            0, L"STATIC", L"Resolution:", WS_CHILD | WS_VISIBLE, 356, 196,
            96, 24, window, reinterpret_cast<HMENU>(IDC_RESOLUTION_LABEL), instance, nullptr);
        SendMessageW(resolutionLabel, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        HWND resolutionCombo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 456, 192, 206, 100,
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
            kInitialStatus,
            WS_CHILD | WS_VISIBLE, 18, 238, 644, 234, window,
            reinterpret_cast<HMENU>(IDC_STATUS), instance, nullptr);
        SendMessageW(statusLabel, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        if (controller) {
            controller->BindControls(statusLabel, fpsCombo, copyError);
            ApplyLanguage(window, *controller);
        }
        SetTimer(window, STATUS_TIMER, 16, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (!controller) return 0;
        if (LOWORD(wParam) == IDC_LANGUAGE &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT selection = SendDlgItemMessageW(
                window, IDC_LANGUAGE, CB_GETCURSEL, 0, 0);
            if (selection == 0 || selection == 1) {
                controller->SetLanguage(selection == 1
                    ? UiLanguage::SimplifiedChinese : UiLanguage::English);
                ApplyLanguage(window, *controller);
                SaveUiLanguage(controller->Language());
            }
        }
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
        if (HANDLE font = RemovePropW(window, L"FcsUiFont")) DeleteObject(font);
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
    constexpr DWORD controlStyle =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, 680, 490};
    AdjustWindowRectEx(&bounds, controlStyle, FALSE, 0);
    HWND controlWindow = CreateWindowExW(
        0, L"FcsControlWindow",
        UiText(controller.Language(), kControlTitle), controlStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance,
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
