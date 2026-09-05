// Hidden-window regression for the controller-owned clean preview. The target
// discovery and injection functions are stubbed below, so this executable can
// never attach to FFXIV or another process.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "../host/fcs_controller.hpp"
#include "../host/fcs_preview_window.hpp"
#include "../host/fcs_stream_resolution.hpp"
#include "../host/fcs_target.hpp"
#include "../host/fcs_ui.hpp"

namespace {

LONG g_findTargetCalls = 0;
LONG g_injectCalls = 0;
LONG g_clipboardWrites = 0;
bool g_clipboardSucceeds = true;
HWND g_dummyTarget = nullptr;
HWND g_clipboardOwner = nullptr;
wchar_t g_clipboardText[2048]{};

// Language changes exercise real persistence without reading or writing the
// user's preferences. Delete only the files/directories created by this test.
struct TemporaryLanguageSettings {
    wchar_t previous[32768]{};
    wchar_t directory[MAX_PATH]{};
    bool active = false;
    bool hadPrevious = false;

    bool Initialize() {
        hadPrevious = GetEnvironmentVariableW(L"LOCALAPPDATA", previous,
                                               _countof(previous)) != 0;
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(_countof(temp), temp);
        if (!length || length >= _countof(temp)) return false;
        if (swprintf_s(directory, L"%sFcsLanguageTest-%lu-%llu", temp,
                       GetCurrentProcessId(), GetTickCount64()) < 0 ||
            !CreateDirectoryW(directory, nullptr)) return false;
        active = SetEnvironmentVariableW(L"LOCALAPPDATA", directory) != FALSE;
        if (!active) RemoveDirectoryW(directory);
        return active;
    }

    ~TemporaryLanguageSettings() {
        if (!active) return;
        SetEnvironmentVariableW(L"LOCALAPPDATA", hadPrevious ? previous : nullptr);
        wchar_t path[MAX_PATH]{};
        swprintf_s(path, L"%s\\FfxivCleanStream\\settings.ini", directory);
        DeleteFileW(path);
        swprintf_s(path, L"%s\\FfxivCleanStream", directory);
        RemoveDirectoryW(path);
        RemoveDirectoryW(directory);
    }
};

bool VerifyLocalization() {
    using namespace fcs::host;
    if (LanguageForWindowsUi(0x0804) != UiLanguage::SimplifiedChinese ||
        LanguageForWindowsUi(0x1004) != UiLanguage::SimplifiedChinese ||
        LanguageForWindowsUi(0x0004) != UiLanguage::SimplifiedChinese ||
        LanguageForWindowsUi(0x0404) != UiLanguage::English ||
        LanguageForWindowsUi(0x0c04) != UiLanguage::English ||
        LanguageForWindowsUi(0x0409) != UiLanguage::English ||
        LanguageForWindowsUi(0x0411) != UiLanguage::English ||
        LoadUiLanguage() != LanguageForWindowsUi(GetUserDefaultUILanguage()) ||
        !SaveUiLanguage(UiLanguage::SimplifiedChinese) ||
        LoadUiLanguage() != UiLanguage::SimplifiedChinese ||
        !SaveUiLanguage(UiLanguage::English) ||
        LoadUiLanguage() != UiLanguage::English) {
        wprintf(L"FAIL: UI language defaults or persistence\n");
        return false;
    }
    const struct { const wchar_t* english; const wchar_t* chinese; } messages[] = {
        {L"Could not get FFXIV's Direct3D 11 device.",
         L"无法获取 FFXIV 的 Direct3D 11 设备。"},
        {L"All GPU-sharing paths failed. Game NT 0x80070001; game legacy 0x80070002; "
         L"host NT name 0x80070003; host NT handle 0x80070004; host legacy 0x80070005; "
         L"plain legacy 0x80070006.",
         L"所有 GPU 共享方式均失败。游戏 NT 0x80070001；游戏传统共享 0x80070002；"
         L"控制器 NT 名称 0x80070003；控制器 NT 句柄 0x80070004；"
         L"控制器传统共享 0x80070005；普通传统共享 0x80070006。"},
        {L"Shared capture setup failed at format selection (0x887A0004). "
         L"Source format 28 -> transport 87, 1920x1080, samples 4, bind flags 0x28.",
         L"共享捕获在格式选择阶段设置失败（0x887A0004）。源格式 28 → 传输格式 87，"
         L"1920×1080，采样数 4，绑定标志 0x28。"},
        {L"Unknown GPU error Ω.\r\n诊断信息。", L"Unknown GPU error Ω.\r\n诊断信息。"},
        {L"All GPU-sharing paths failed. Incomplete diagnostic.",
         L"All GPU-sharing paths failed. Incomplete diagnostic."},
    };
    for (const auto& message : messages) {
        wchar_t text[512]{};
        LocalizeDiagnostic(UiLanguage::SimplifiedChinese, message.english,
                           text, _countof(text));
        if (wcscmp(text, message.chinese) != 0) {
            wprintf(L"FAIL: translated diagnostic: %ls\n", text);
            return false;
        }
        LocalizeDiagnostic(UiLanguage::English, message.english, text, _countof(text));
        if (wcscmp(text, message.english) != 0) return false;
    }
    return true;
}

bool CaptureClipboardText(HWND owner, const wchar_t* text) {
    ++g_clipboardWrites;
    g_clipboardOwner = owner;
    wcsncpy_s(g_clipboardText,
              sizeof(g_clipboardText) / sizeof(g_clipboardText[0]),
              text ? text : L"", _TRUNCATE);
    return g_clipboardSucceeds && text && text[0];
}

LRESULT CALLBACK DummyTargetWindowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateHiddenDummyTarget(HINSTANCE instance) {
    constexpr wchar_t className[] = L"FcsHostPreviewLifecycleDummyTarget";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DummyTargetWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    return CreateWindowExW(
        WS_EX_TOOLWINDOW, className, L"FCS hidden lifecycle target",
        WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
}

struct ThreadWindowSearch {
    const wchar_t* className;
    HWND window;
    UINT matches;
};

BOOL CALLBACK FindThreadWindowByClass(HWND window, LPARAM lParam) {
    auto* search = reinterpret_cast<ThreadWindowSearch*>(lParam);
    wchar_t className[64]{};
    if (!GetClassNameW(window, className,
                       static_cast<int>(sizeof(className) /
                                        sizeof(className[0]))) ||
        lstrcmpW(className, search->className) != 0) {
        return TRUE;
    }
    if (!search->window) search->window = window;
    ++search->matches;
    return TRUE;
}

HWND FindUniqueCurrentThreadWindow(const wchar_t* className) {
    ThreadWindowSearch search{className, nullptr, 0};
    EnumThreadWindows(GetCurrentThreadId(), FindThreadWindowByClass,
                      reinterpret_cast<LPARAM>(&search));
    return search.matches == 1 ? search.window : nullptr;
}

HWND FindChildByText(HWND parent, const wchar_t* className,
                     const wchar_t* text) {
    HWND child = nullptr;
    while ((child = FindWindowExW(parent, child, className, nullptr)) !=
           nullptr) {
        wchar_t childText[256]{};
        GetWindowTextW(child, childText,
                       static_cast<int>(sizeof(childText) /
                                        sizeof(childText[0])));
        if (lstrcmpW(childText, text) == 0) return child;
    }
    return nullptr;
}

HWND FindResolutionCombo(HWND parent) {
    return GetDlgItem(parent, 1006);
}

bool ExpectText(HWND window, const wchar_t* expected) {
    wchar_t text[2048]{};
    return window && GetWindowTextW(window, text, _countof(text)) &&
           wcscmp(text, expected) == 0;
}

bool VerifyTextFits(HWND control) {
    for (HWND child = GetWindow(control, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t className[32]{}, text[2048]{};
        GetClassNameW(child, className, _countof(className));
        const bool button = _wcsicmp(className, L"Button") == 0;
        const bool combo = _wcsicmp(className, L"ComboBox") == 0;
        if (!button && !combo && _wcsicmp(className, L"Static") != 0) continue;
        GetWindowTextW(child, text, _countof(text));
        RECT client{};
        GetClientRect(child, &client);
        RECT measured{0, 0, client.right - (button ? 16 : 0), 0};
        HDC dc = GetDC(child);
        HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(
            SendMessageW(child, WM_GETFONT, 0, 0)));
        if (combo) {
            bool fits = true;
            const LRESULT count = SendMessageW(child, CB_GETCOUNT, 0, 0);
            for (LRESULT index = 0; index < count; ++index) {
                SendMessageW(child, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
                SIZE size{};
                GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
                if (size.cx > client.right - GetSystemMetrics(SM_CXVSCROLL) - 8) {
                    wprintf(L"FAIL: combo text is clipped: %ls\n", text);
                    fits = false;
                }
            }
            SelectObject(dc, old);
            ReleaseDC(child, dc);
            if (!fits) return false;
            continue;
        }
        DrawTextW(dc, text, -1, &measured,
                  DT_CALCRECT | DT_NOPREFIX | (button ? DT_SINGLELINE : DT_WORDBREAK));
        SelectObject(dc, old);
        ReleaseDC(child, dc);
        if (measured.right > client.right - (button ? 16 : 0) ||
            measured.bottom > client.bottom) {
            wprintf(L"FAIL: UI text is clipped: %ls\n", text);
            return false;
        }
    }
    return true;
}

bool DispatchControlNotification(HWND control, HWND child,
                                 WORD notification) {
    if (!control || !child) return false;
    const int identifier = GetDlgCtrlID(child);
    if (identifier <= 0) return false;
    SendMessageW(control, WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(identifier), notification),
                 reinterpret_cast<LPARAM>(child));
    return true;
}

bool SwitchLanguage(HWND control, fcs::host::HostController& controller,
                     fcs::host::UiLanguage language) {
    using namespace fcs::host;
    HWND combo = GetDlgItem(control, 1008);
    HWND fps = GetDlgItem(control, 1003);
    const LRESULT fpsSelection = SendMessageW(fps, CB_GETCURSEL, 0, 0);
    const auto resolution = controller.SelectedStreamResolution();
    const HWND preview = controller.PreviewWindow();
    const LONG finds = g_findTargetCalls, injections = g_injectCalls;
    const int index = language == UiLanguage::SimplifiedChinese ? 1 : 0;
    if (!combo || SendMessageW(combo, CB_GETCOUNT, 0, 0) != 2 ||
        SendMessageW(combo, CB_SETCURSEL, index, 0) != index ||
        !DispatchControlNotification(control, combo, CBN_SELCHANGE) ||
        controller.Language() != language || LoadUiLanguage() != language ||
        !ExpectText(GetDlgItem(control, 1001), UiText(language, L"Start / Resume")) ||
        !ExpectText(GetDlgItem(control, 1002), UiText(language, L"Pause capture")) ||
        !ExpectText(GetDlgItem(control, 1005), UiText(language, L"End stream")) ||
        !ExpectText(control, UiText(language, kControlTitle)) ||
        controller.PreviewWindow() != preview ||
        controller.SelectedStreamResolution() != resolution ||
        SendMessageW(fps, CB_GETCURSEL, 0, 0) != fpsSelection ||
        g_findTargetCalls != finds || g_injectCalls != injections ||
        !VerifyTextFits(control)) {
        wprintf(L"FAIL: live language switch changed capture/settings or UI text\n");
        return false;
    }
    wchar_t option[128]{};
    SendMessageW(fps, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(option));
    return wcscmp(option, UiText(language, L"30 FPS (recommended)")) == 0 &&
           (!preview || ExpectText(preview, L"FFXIV Clean Stream"));
}

bool OpenCaptureIpc(HANDLE& mapping, FcsIpcV1*& ipc) {
    mapping = nullptr;
    ipc = nullptr;
    wchar_t name[96]{};
    FcsMappingName(name, GetCurrentProcessId());
    HANDLE openedMapping =
        OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!openedMapping) return false;
    FcsIpcV1* openedIpc = static_cast<FcsIpcV1*>(MapViewOfFile(
        openedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FcsIpcV1)));
    if (!openedIpc || openedIpc->magic != FCS_MAGIC ||
        openedIpc->version != FCS_VERSION ||
        openedIpc->bytes != sizeof(FcsIpcV1) ||
        openedIpc->controllerPid != GetCurrentProcessId() ||
        openedIpc->targetHwnd !=
            reinterpret_cast<uint64_t>(g_dummyTarget)) {
        if (openedIpc) UnmapViewOfFile(openedIpc);
        CloseHandle(openedMapping);
        return false;
    }
    mapping = openedMapping;
    ipc = openedIpc;
    return true;
}

bool ExpectCaptureControl(FcsIpcV1* ipc, LONG expectedStop,
                          bool expectRecreate, const wchar_t* stage) {
    const bool recreate = ipc &&
        (ipc->command & FCS_COMMAND_RECREATE_RESOURCES) != 0;
    if (!ipc || ipc->controllerPid != GetCurrentProcessId() ||
        ipc->targetHwnd != reinterpret_cast<uint64_t>(g_dummyTarget) ||
        ipc->stopRequested != expectedStop || recreate != expectRecreate) {
        wprintf(L"FAIL: %ls IPC state mismatch (stop=%ld, recreate=%u)\n",
                stage, ipc ? ipc->stopRequested : -1L,
                static_cast<unsigned int>(recreate));
        return false;
    }
    return true;
}

bool ExpectHidden(HWND window, const wchar_t* stage) {
    if (window && IsWindow(window) && !IsWindowVisible(window)) return true;
    wprintf(L"FAIL: %ls is missing or visible\n", stage);
    return false;
}

bool ExpectPerMonitorV2(HWND window, const wchar_t* stage) {
    if (window && IsWindow(window) &&
        AreDpiAwarenessContextsEqual(
            GetWindowDpiAwarenessContext(window),
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return true;
    }
    wprintf(L"FAIL: %ls is not per-monitor-v2 DPI aware\n", stage);
    return false;
}

bool ExpectClientSize(HWND window, uint32_t expectedWidth,
                      uint32_t expectedHeight, const wchar_t* stage) {
    if (!window || !IsWindow(window)) {
        wprintf(L"FAIL: %ls has no live preview window\n", stage);
        return false;
    }

    const DPI_AWARENESS_CONTEXT windowContext =
        GetWindowDpiAwarenessContext(window);
    const DPI_AWARENESS_CONTEXT previousContext = windowContext
        ? SetThreadDpiAwarenessContext(windowContext)
        : nullptr;
    RECT client{};
    const BOOL readClient = GetClientRect(window, &client);
    if (previousContext) SetThreadDpiAwarenessContext(previousContext);
    if (!windowContext || !previousContext || !readClient) {
        wprintf(L"FAIL: %ls has no live preview window\n", stage);
        return false;
    }

    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width != static_cast<LONG>(expectedWidth) ||
        height != static_cast<LONG>(expectedHeight)) {
        wprintf(L"FAIL: %ls client area is %ldx%ld, expected %ux%u\n",
                stage, width, height, expectedWidth, expectedHeight);
        return false;
    }
    return true;
}

bool VerifyResolutionPresets() {
    using fcs::host::IsSupportedStreamResolution;
    using fcs::host::StreamResolution;
    using fcs::host::StreamResolutionDimensions;

    const auto hd = StreamResolutionDimensions(StreamResolution::Hd720);
    const auto fullHd =
        StreamResolutionDimensions(StreamResolution::FullHd1080);
    if (fcs::host::kDefaultStreamResolution != StreamResolution::Hd720 ||
        !IsSupportedStreamResolution(StreamResolution::Hd720) ||
        !IsSupportedStreamResolution(StreamResolution::FullHd1080) ||
        IsSupportedStreamResolution(static_cast<StreamResolution>(2)) ||
        IsSupportedStreamResolution(
            static_cast<StreamResolution>(UINT32_MAX)) ||
        hd.width != 1280 || hd.height != 720 ||
        fullHd.width != 1920 || fullHd.height != 1080 ||
        hd.width * 9u != hd.height * 16u ||
        fullHd.width * 9u != fullHd.height * 16u) {
        wprintf(L"FAIL: the stream-resolution preset table changed\n");
        return false;
    }
    return true;
}

} // namespace

namespace fcs::host {

bool FindFfxiv(TargetInfo& result) {
    ++g_findTargetCalls;
    if (!g_dummyTarget || !IsWindow(g_dummyTarget)) {
        result = {};
        return false;
    }
    result.pid = GetCurrentProcessId();
    result.window = g_dummyTarget;
    return true;
}

bool InjectHook(DWORD pid, wchar_t*, size_t) {
    ++g_injectCalls;
    return pid == GetCurrentProcessId() && g_injectCalls == 1;
}

} // namespace fcs::host

int main() {
    using fcs::host::HostController;
    using fcs::host::StreamResolution;
    using fcs::host::UiLanguage;

    TemporaryLanguageSettings settings;
    if (!settings.Initialize() || !VerifyLocalization()) return 1;
    if (!VerifyResolutionPresets()) return 1;

    // Match the production process before any HWND is created. All preview
    // windows remain hidden throughout this test.
    SetProcessDPIAware();

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!instance) {
        wprintf(L"FAIL: could not identify the test module\n");
        return 1;
    }

    g_dummyTarget = CreateHiddenDummyTarget(instance);
    if (!g_dummyTarget || IsWindowVisible(g_dummyTarget)) {
        wprintf(L"FAIL: could not create a hidden in-process target\n");
        if (g_dummyTarget) DestroyWindow(g_dummyTarget);
        g_dummyTarget = nullptr;
        return 1;
    }

    HostController controller(instance, &CaptureClipboardText);
    if (!fcs::host::CreateHostWindows(instance, SW_HIDE, controller)) {
        wprintf(L"FAIL: could not create the hidden host windows\n");
        DestroyWindow(g_dummyTarget);
        g_dummyTarget = nullptr;
        return 1;
    }

    HWND control = FindUniqueCurrentThreadWindow(L"FcsControlWindow");
    HWND preview = controller.PreviewWindow();
    HWND startButton = control
        ? FindChildByText(control, L"BUTTON", L"Start / Resume")
        : nullptr;
    HWND endButton = control
        ? FindChildByText(control, L"BUTTON", L"End stream")
        : nullptr;
    HWND copyErrorButton = control
        ? FindChildByText(control, L"BUTTON", L"Copy error")
        : nullptr;
    HWND resolutionCombo = control ? FindResolutionCombo(control) : nullptr;
    HWND statusLabel = control
        ? FindChildByText(
              control, L"STATIC",
              L"Nothing is attached. This app will not touch FFXIV until you click Start / Resume.")
        : nullptr;
    HANDLE captureMapping = nullptr;
    FcsIpcV1* captureIpc = nullptr;
    bool passed = true;

    if (!ExpectHidden(control, L"control window") ||
        !ExpectHidden(preview, L"initial preview") ||
        !ExpectHidden(g_dummyTarget, L"dummy target") || !startButton ||
        !endButton || !copyErrorButton || !resolutionCombo || !statusLabel ||
        !ExpectPerMonitorV2(preview, L"initial preview")) {
        wprintf(L"FAIL: hidden UI controls were not created as expected\n");
        passed = false;
    }
    if (!controller.HasPreviewWindow() ||
        controller.PreviewWindow() != preview ||
        !ExpectClientSize(preview, 1280, 720, L"default preset") ||
        (copyErrorButton && IsWindowEnabled(copyErrorButton)) ||
        (resolutionCombo &&
         SendMessageW(resolutionCombo, CB_GETCURSEL, 0, 0) != 0)) {
        passed = false;
    }
    if (copyErrorButton) {
        DispatchControlNotification(control, copyErrorButton, BN_CLICKED);
    }
    if (g_clipboardWrites != 0) {
        wprintf(L"FAIL: disabled Copy error exposed stale status text\n");
        passed = false;
    }

    // Exercise the actual combo-box notification rather than calling the
    // controller directly. Control IDs distinguish resolution and language.
    if (!resolutionCombo ||
        SendMessageW(resolutionCombo, CB_SETCURSEL, 1, 0) != 1 ||
        !DispatchControlNotification(control, resolutionCombo,
                                     CBN_SELCHANGE) ||
        controller.SelectedStreamResolution() !=
            StreamResolution::FullHd1080 ||
        !ExpectClientSize(preview, 1920, 1080, L"1080p combo preset") ||
        !ExpectHidden(preview, L"resized preview")) {
        wprintf(L"FAIL: resolution combo did not dispatch the 1080p preset\n");
        passed = false;
    }

    HWND fpsCombo = GetDlgItem(control, 1003);
    SendMessageW(fpsCombo, CB_SETCURSEL, 2, 0);
    if (!SwitchLanguage(control, controller, UiLanguage::SimplifiedChinese) ||
        !ExpectText(statusLabel, L"尚未连接游戏。点击“开始 / 继续”后，本应用才会连接 FFXIV。") ||
        !SwitchLanguage(control, controller, UiLanguage::English)) {
        passed = false;
    }
    SendMessageW(fpsCombo, CB_SETCURSEL, 0, 0);

    RECT dpiSuggested{};
    const UINT previewDpi = preview ? GetDpiForWindow(preview) : 0;
    if (!preview || !previewDpi || !GetWindowRect(preview, &dpiSuggested) ||
        SendMessageW(preview, WM_DPICHANGED,
                     MAKEWPARAM(static_cast<WORD>(previewDpi),
                                static_cast<WORD>(previewDpi)),
                     reinterpret_cast<LPARAM>(&dpiSuggested)) != 0 ||
        !ExpectClientSize(preview, 1920, 1080,
                          L"same-DPI 1080p preset") ||
        !ExpectHidden(preview, L"DPI-adjusted preview")) {
        wprintf(L"FAIL: DPI dispatch did not preserve the selected client size\n");
        passed = false;
    }

    // Establish a real CaptureSession against this test process and its
    // private dummy HWND. Injection is stubbed to succeed without loading a
    // module or touching another process.
    if (!DispatchControlNotification(control, startButton, BN_CLICKED) ||
        g_findTargetCalls != 1 || g_injectCalls != 1 ||
        !OpenCaptureIpc(captureMapping, captureIpc) ||
        !ExpectCaptureControl(captureIpc, 0, true, L"initial Start") ||
        (copyErrorButton && IsWindowEnabled(copyErrorButton))) {
        wprintf(L"FAIL: Start button did not establish the safe test session "
                L"(find=%ld, inject=%ld)\n",
                g_findTargetCalls, g_injectCalls);
        passed = false;
    }
    if (captureIpc) {
        InterlockedAnd(
            &captureIpc->command,
            ~static_cast<LONG>(FCS_COMMAND_RECREATE_RESOURCES));
    }

    // Statistics and helper messages use the same snapshot in both languages.
    if (captureIpc) {
        captureIpc->copiesSubmitted = 12345;
        captureIpc->busyDropped = 9;
        captureIpc->qpcFrequency = 1000000;
        captureIpc->hookCpuTicksMax = 25;
        wcsncpy_s(captureIpc->message,
                  L"Clean frames are reaching the standalone preview.", _TRUNCATE);
        InterlockedExchange(&captureIpc->state, FCS_STATE_STREAMING);
    }
    for (UINT tick = 0; tick < 30; ++tick) controller.Tick();
    if (!SwitchLanguage(control, controller, UiLanguage::SimplifiedChinese) ||
        !ExpectText(statusLabel,
            L"已就绪。请在 Discord 中共享“FFXIV Clean Stream”窗口。\r\n\r\n"
            L"纯净画面帧数：12345    因忙碌丢弃的帧数：9\r\n"
            L"帧复制提交的最长 CPU 耗时：25.0 微秒\r\n\r\n"
            L"纯净画面正在传送到独立预览窗口。") ||
        !ExpectCaptureControl(captureIpc, 0, false, L"language switch while streaming") ||
        !DispatchControlNotification(control, GetDlgItem(control, 1002), BN_CLICKED) ||
        !ExpectText(statusLabel, L"已暂停 GPU 帧复制。点击“开始 / 继续”可恢复捕获。") ||
        !SwitchLanguage(control, controller, UiLanguage::English) ||
        !ExpectText(statusLabel, L"GPU frame copies are paused. Click Start / Resume to resume.") ||
        !ExpectCaptureControl(captureIpc, 1, false, L"language switch while paused")) {
        wprintf(L"FAIL: streaming statistics or paused language switch\n");
        passed = false;
    }
    controller.StartCapture();
    if (captureIpc) {
        InterlockedAnd(&captureIpc->command,
                       ~static_cast<LONG>(FCS_COMMAND_RECREATE_RESOURCES));
    }

    // Publish a distinctive multiline Unicode error through the real IPC
    // status path. The injected writer captures it in memory and never opens
    // or changes the user's clipboard.
    constexpr wchar_t rawError[] =
        L"Synthetic GPU error Ω.\r\nSecond diagnostic line.";
    constexpr wchar_t displayedErrorExpected[] =
        L"Capture error (-4242):\r\nSynthetic GPU error Ω.\r\n"
        L"Second diagnostic line.";
    wchar_t displayedError[256]{};
    wchar_t copyButtonText[64]{};
    if (captureIpc) {
        captureIpc->lastError = -4242;
        wcsncpy_s(captureIpc->message,
                  sizeof(captureIpc->message) /
                      sizeof(captureIpc->message[0]),
                  rawError, _TRUNCATE);
        InterlockedExchange(&captureIpc->state, FCS_STATE_ERROR);
    }
    for (UINT tick = 0; tick < 30; ++tick) controller.Tick();
    if (statusLabel) {
        GetWindowTextW(statusLabel, displayedError,
                       static_cast<int>(sizeof(displayedError) /
                                        sizeof(displayedError[0])));
    }
    g_clipboardSucceeds = false;
    if (!copyErrorButton || !IsWindowEnabled(copyErrorButton) ||
        lstrcmpW(displayedError, displayedErrorExpected) != 0 ||
        !DispatchControlNotification(control, copyErrorButton, BN_CLICKED)) {
        wprintf(L"FAIL: Copy error was not enabled for an IPC error\n");
        passed = false;
    }
    if (copyErrorButton) {
        GetWindowTextW(copyErrorButton, copyButtonText,
                       static_cast<int>(sizeof(copyButtonText) /
                                        sizeof(copyButtonText[0])));
    }
    if (statusLabel) {
        GetWindowTextW(statusLabel, displayedError,
                       static_cast<int>(sizeof(displayedError) /
                                        sizeof(displayedError[0])));
    }
    if (g_clipboardWrites != 1 || g_clipboardOwner != control ||
        lstrcmpW(g_clipboardText, displayedErrorExpected) != 0 ||
        lstrcmpW(copyButtonText, L"Copy failed") != 0 ||
        !IsWindowEnabled(copyErrorButton) ||
        lstrcmpW(displayedError, displayedErrorExpected) != 0) {
        wprintf(L"FAIL: failed clipboard write lost the diagnostic or retry\n");
        passed = false;
    }
    g_clipboardSucceeds = true;
    if (!DispatchControlNotification(control, copyErrorButton, BN_CLICKED)) {
        wprintf(L"FAIL: Copy error retry dispatch failed\n");
        passed = false;
    }
    if (copyErrorButton) {
        GetWindowTextW(copyErrorButton, copyButtonText,
                       static_cast<int>(sizeof(copyButtonText) /
                                        sizeof(copyButtonText[0])));
    }
    if (g_clipboardWrites != 2 ||
        lstrcmpW(g_clipboardText, displayedErrorExpected) != 0 ||
        lstrcmpW(copyButtonText, L"Copied!") != 0) {
        wprintf(L"FAIL: Copy error did not copy the complete diagnostic\n");
        passed = false;
    }

    // Re-render the retained error immediately, without a Tick or a capture
    // restart. Copy error must follow the visible language and preserve Unicode.
    constexpr wchar_t chineseError[] =
        L"捕获错误（-4242）：\r\nSynthetic GPU error Ω.\r\nSecond diagnostic line.";
    if (!SwitchLanguage(control, controller, UiLanguage::SimplifiedChinese) ||
        !ExpectText(statusLabel, chineseError) || !IsWindowEnabled(copyErrorButton) ||
        !DispatchControlNotification(control, copyErrorButton, BN_CLICKED) ||
        g_clipboardWrites != 3 || wcscmp(g_clipboardText, chineseError) != 0 ||
        !ExpectText(copyErrorButton, L"已复制！") ||
        !SwitchLanguage(control, controller, UiLanguage::English) ||
        !ExpectText(statusLabel, displayedErrorExpected) ||
        !ExpectCaptureControl(captureIpc, 0, false, L"language switch during error")) {
        wprintf(L"FAIL: language switch lost the current copyable diagnostic\n");
        passed = false;
    }

    // Any informational update must clear the copyable diagnostic and
    // disable the button, including against a forged WM_COMMAND.
    if (!DispatchControlNotification(control, startButton, BN_CLICKED) ||
        g_findTargetCalls != 1 || g_injectCalls != 1 ||
        !ExpectCaptureControl(captureIpc, 0, true,
                              L"resume after copied error") ||
        (copyErrorButton && IsWindowEnabled(copyErrorButton))) {
        wprintf(L"FAIL: informational resume retained the copyable error\n");
        passed = false;
    }
    if (copyErrorButton) {
        DispatchControlNotification(control, copyErrorButton, BN_CLICKED);
        GetWindowTextW(copyErrorButton, copyButtonText,
                       static_cast<int>(sizeof(copyButtonText) /
                                        sizeof(copyButtonText[0])));
    }
    if (g_clipboardWrites != 3 ||
        lstrcmpW(copyButtonText, L"Copy error") != 0) {
        wprintf(L"FAIL: disabled Copy error copied a stale diagnostic\n");
        passed = false;
    }
    if (captureIpc) {
        InterlockedExchange(&captureIpc->state, FCS_STATE_INJECTED);
        InterlockedAnd(
            &captureIpc->command,
            ~static_cast<LONG>(FCS_COMMAND_RECREATE_RESOURCES));
    }

    // End through the real button dispatch. Tick must remain inert with no
    // preview and leave the retained session stopped.
    const HWND buttonEndedPreview = controller.PreviewWindow();
    if (!DispatchControlNotification(control, endButton, BN_CLICKED) ||
        controller.HasPreviewWindow() || controller.PreviewWindow() ||
        (buttonEndedPreview && IsWindow(buttonEndedPreview)) ||
        !ExpectCaptureControl(captureIpc, 1, false, L"End button")) {
        wprintf(L"FAIL: End button left the hidden preview or capture active\n");
        passed = false;
    }
    for (UINT tick = 0; tick < 64; ++tick) controller.Tick();
    if (controller.HasPreviewWindow() ||
        !ExpectCaptureControl(captureIpc, 1, false, L"Tick after End")) {
        passed = false;
    }

    // Choose 720p while ended, then take the retained-session Resume path via
    // the Start button. Discovery and injection must not run a second time.
    if (!resolutionCombo ||
        SendMessageW(resolutionCombo, CB_SETCURSEL, 0, 0) != 0 ||
        !DispatchControlNotification(control, resolutionCombo,
                                     CBN_SELCHANGE) ||
        controller.SelectedStreamResolution() != StreamResolution::Hd720 ||
        !DispatchControlNotification(control, startButton, BN_CLICKED)) {
        wprintf(L"FAIL: ended-session settings or Start dispatch failed\n");
        passed = false;
    }
    HWND resumedPreview = controller.PreviewWindow();
    if (!controller.HasPreviewWindow() || !resumedPreview ||
         !ExpectClientSize(resumedPreview, 1280, 720,
                           L"resumed 720p preset") ||
         !ExpectHidden(resumedPreview, L"resumed preview") ||
         !ExpectPerMonitorV2(resumedPreview, L"resumed preview") ||
        g_findTargetCalls != 1 || g_injectCalls != 1 ||
        !ExpectCaptureControl(captureIpc, 0, true, L"Start resume")) {
        wprintf(L"FAIL: retained Start repeated discovery/injection or did "
                L"not restore the preview (find=%ld, inject=%ld)\n",
                g_findTargetCalls, g_injectCalls);
        passed = false;
    }
    if (captureIpc) {
        InterlockedAnd(
            &captureIpc->command,
            ~static_cast<LONG>(FCS_COMMAND_RECREATE_RESOURCES));
    }

    // A synchronous WM_CLOSE is the preview title-bar X path. It must route
    // through EndCapture without destroying the hidden controls window.
    if (!resumedPreview ||
        SendMessageW(resumedPreview, WM_CLOSE, 0, 0) != 0 ||
        controller.HasPreviewWindow() || controller.PreviewWindow() ||
        IsWindow(resumedPreview) || !ExpectHidden(control, L"control after X") ||
        !ExpectCaptureControl(captureIpc, 1, false, L"preview X") ||
        g_findTargetCalls != 1 || g_injectCalls != 1) {
        wprintf(L"FAIL: preview X did not end the retained stream safely\n");
        passed = false;
    }

    // Cover the defensive NC-destroy path separately from the normal X. A
    // direct destruction must also stop the retained capture because no
    // consumer window remains.
    if (!DispatchControlNotification(control, startButton, BN_CLICKED)) {
        wprintf(L"FAIL: Start dispatch after preview X failed\n");
        passed = false;
    }
    HWND unexpectedlyDestroyedPreview = controller.PreviewWindow();
    if (!ExpectHidden(unexpectedlyDestroyedPreview,
                      L"preview before direct destruction") ||
        !ExpectCaptureControl(captureIpc, 0, true,
                              L"resume after preview X")) {
        passed = false;
    }
    if (captureIpc) {
        InterlockedAnd(
            &captureIpc->command,
            ~static_cast<LONG>(FCS_COMMAND_RECREATE_RESOURCES));
    }
    if (!unexpectedlyDestroyedPreview ||
        !DestroyWindow(unexpectedlyDestroyedPreview) ||
        controller.HasPreviewWindow() || controller.PreviewWindow() ||
        IsWindow(unexpectedlyDestroyedPreview) ||
        !ExpectCaptureControl(captureIpc, 1, false,
                              L"unexpected preview destruction") ||
        g_findTargetCalls != 1 || g_injectCalls != 1) {
        wprintf(L"FAIL: direct preview destruction left capture active\n");
        passed = false;
    }
    wchar_t endedStatus[256]{};
    wchar_t statusAfterTick[256]{};
    if (statusLabel) {
        GetWindowTextW(statusLabel, endedStatus,
                       static_cast<int>(sizeof(endedStatus) /
                                        sizeof(endedStatus[0])));
    }
    // Cross the controller's periodic status-update boundary. A single Tick
    // would not prove that the no-preview guard keeps the ended text stable.
    for (UINT tick = 0; tick < 64; ++tick) controller.Tick();
    if (statusLabel) {
        GetWindowTextW(statusLabel, statusAfterTick,
                       static_cast<int>(sizeof(statusAfterTick) /
                                        sizeof(statusAfterTick[0])));
    }
    if (!endedStatus[0] || !wcsstr(endedStatus, L"Stream ended.") ||
        lstrcmpW(endedStatus, statusAfterTick) != 0) {
        wprintf(L"FAIL: Tick overwrote the direct-destroy ended status\n");
        passed = false;
    }

    if (!SwitchLanguage(control, controller, UiLanguage::SimplifiedChinese) ||
        !ExpectText(statusLabel,
            L"直播已结束，纯净画面预览窗口已关闭。点击“开始 / 继续”可重新打开。") ||
        IsWindowEnabled(copyErrorButton) ||
        !ExpectCaptureControl(captureIpc, 1, false, L"language switch after End")) {
        wprintf(L"FAIL: ended status did not switch language immediately\n");
        passed = false;
    }

    if (control && IsWindow(control)) DestroyWindow(control);
    controller.Shutdown();
    const LONG findCallsBeforeShutdownStart = g_findTargetCalls;
    const LONG injectCallsBeforeShutdownStart = g_injectCalls;
    controller.StartCapture();
    if (controller.HasPreviewWindow() ||
        FindUniqueCurrentThreadWindow(L"FcsControlWindow") ||
        g_findTargetCalls != findCallsBeforeShutdownStart ||
        g_injectCalls != injectCallsBeforeShutdownStart) {
        wprintf(L"FAIL: shutdown allowed the host to restart or left a "
                L"window alive\n");
        passed = false;
    }
    if (captureIpc) UnmapViewOfFile(captureIpc);
    if (captureMapping) CloseHandle(captureMapping);

    // A fresh controller uses the saved language before constructing controls.
    HostController reopened(instance, &CaptureClipboardText);
    reopened.SetLanguage(fcs::host::LoadUiLanguage());
    if (!fcs::host::CreateHostWindows(instance, SW_HIDE, reopened)) {
        passed = false;
    }
    HWND reopenedControl = FindUniqueCurrentThreadWindow(L"FcsControlWindow");
    if (!reopenedControl || reopened.Language() != UiLanguage::SimplifiedChinese ||
        !ExpectText(GetDlgItem(reopenedControl, 1001), L"开始 / 继续") ||
        !VerifyTextFits(reopenedControl)) {
        wprintf(L"FAIL: saved Simplified Chinese startup\n");
        passed = false;
    }
    if (reopenedControl) DestroyWindow(reopenedControl);
    reopened.Shutdown();
    DestroyWindow(g_dummyTarget);
    g_dummyTarget = nullptr;

    if (!passed) return 1;
    wprintf(L"PASS: hidden UI dispatch, English/Simplified Chinese switching, "
            L"language persistence, diagnostics, text layout, Copy error, preview X, "
            L"retained resume, DPI, and 16:9 resolution presets\n");
    return 0;
}
