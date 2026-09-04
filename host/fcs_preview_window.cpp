#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fcs_controller.hpp"
#include "fcs_preview_window.hpp"

namespace fcs::host {
namespace {

constexpr wchar_t PREVIEW_CLASS_NAME[] = L"FcsPreviewWindow";
constexpr DWORD PREVIEW_STYLE =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

class ScopedPreviewDpiAwareness final {
public:
    ScopedPreviewDpiAwareness()
        : previous_(SetThreadDpiAwarenessContext(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

    ~ScopedPreviewDpiAwareness() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }

    ScopedPreviewDpiAwareness(const ScopedPreviewDpiAwareness&) = delete;
    ScopedPreviewDpiAwareness& operator=(
        const ScopedPreviewDpiAwareness&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

bool AdjustPreviewRect(RECT& rect, DWORD style, DWORD extendedStyle,
                       UINT dpi) {
    return AdjustWindowRectExForDpi(
               &rect, style, FALSE, extendedStyle, dpi) != FALSE;
}

bool ResizePreviewWindowClientAtDpi(
    HWND window, StreamResolution resolution, UINT dpi,
    const RECT* suggestedPosition) {
    if (!window || !IsWindow(window) || !dpi ||
        !IsSupportedStreamResolution(resolution)) {
        return false;
    }

    ScopedPreviewDpiAwareness dpiScope;
    const StreamResolutionSize size =
        StreamResolutionDimensions(resolution);
    const DWORD style = static_cast<DWORD>(
        GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD extendedStyle = static_cast<DWORD>(
        GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT rect{0, 0, static_cast<LONG>(size.width),
              static_cast<LONG>(size.height)};
    if (!AdjustPreviewRect(rect, style, extendedStyle, dpi)) return false;

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int x = 0;
    int y = 0;
    if (suggestedPosition) {
        x = suggestedPosition->left;
        y = suggestedPosition->top;
    } else {
        flags |= SWP_NOMOVE;
    }
    return SetWindowPos(window, nullptr, x, y, rect.right - rect.left,
                        rect.bottom - rect.top, flags) != FALSE;
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

LRESULT CALLBACK PreviewWindowProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    if (message == WM_NCCREATE && !BindController(window, lParam)) {
        return FALSE;
    }

    HostController* controller = ControllerForWindow(window);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (!controller || !controller->HasPreviewDevice()) {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(dc, &rect,
                     static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(210, 210, 210));
            DrawTextW(dc, L"Waiting for a clean FFXIV frame...", -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        if (controller) {
            controller->EndCapture();
        } else {
            DestroyWindow(window);
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        const StreamResolution resolution = controller
            ? controller->SelectedStreamResolution()
            : kDefaultStreamResolution;
        ResizePreviewWindowClientAtDpi(
            window, resolution, HIWORD(wParam), suggested);
        return 0;
    }
    case WM_NCDESTROY:
        if (controller) controller->OnPreviewWindowDestroyed(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterPreviewWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = PreviewWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = PREVIEW_CLASS_NAME;
    if (RegisterClassExW(&windowClass)) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

HWND CreatePreviewWindow(HINSTANCE instance, HostController& controller,
                         StreamResolution resolution) {
    ScopedPreviewDpiAwareness dpiScope;
    if (!IsSupportedStreamResolution(resolution) ||
        !RegisterPreviewWindowClass(instance)) {
        return nullptr;
    }

    const StreamResolutionSize size =
        StreamResolutionDimensions(resolution);
    RECT rect{0, 0, static_cast<LONG>(size.width),
              static_cast<LONG>(size.height)};
    if (!AdjustPreviewRect(rect, PREVIEW_STYLE, 0, GetDpiForSystem())) {
        return nullptr;
    }

    HWND window = CreateWindowExW(
        0, PREVIEW_CLASS_NAME, L"FFXIV Clean Stream", PREVIEW_STYLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr, instance, &controller);
    if (window) ResizePreviewWindowClient(window, resolution);
    return window;
}

bool ResizePreviewWindowClient(HWND window, StreamResolution resolution) {
    if (!window || !IsWindow(window) ||
        !IsSupportedStreamResolution(resolution)) {
        return false;
    }
    return ResizePreviewWindowClientAtDpi(
        window, resolution, GetDpiForWindow(window), nullptr);
}

} // namespace fcs::host
