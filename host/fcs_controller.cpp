#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wchar.h>

#include "fcs_clipboard.hpp"
#include "fcs_controller.hpp"
#include "fcs_preview_window.hpp"
#include "fcs_target.hpp"

namespace fcs::host {

HostController::HostController(HINSTANCE instance)
    : HostController(instance, &CopyUnicodeTextToClipboard) {}

HostController::HostController(HINSTANCE instance,
                               ClipboardWriter clipboardWriter)
    : instance_(instance ? instance : GetModuleHandleW(nullptr)),
      clipboardWriter_(clipboardWriter ? clipboardWriter
                                       : &CopyUnicodeTextToClipboard) {}

HostController::~HostController() {
    Shutdown();
}

void HostController::BindPreviewWindow(HWND window) {
    previewWindow_ = window;
    preview_.SetOutputWindow(window);
}

void HostController::OnPreviewWindowDestroyed(HWND window) {
    if (window != previewWindow_) return;
    const bool expected =
        window == previewDestructionInProgress_ || shuttingDown_;
    preview_.Reset();
    preview_.SetOutputWindow(nullptr);
    previewWindow_ = nullptr;
    if (window == previewDestructionInProgress_) {
        previewDestructionInProgress_ = nullptr;
    }
    if (!expected) {
        session_.RequestStop();
        statusTicks_ = 0;
        SetEndedStatus();
    }
}

bool HostController::HasPreviewWindow() const {
    return previewWindow_ && IsWindow(previewWindow_);
}

void HostController::SetStreamResolution(StreamResolution resolution) {
    if (!IsSupportedStreamResolution(resolution)) return;
    streamResolution_ = resolution;
    if (HasPreviewWindow()) {
        ResizePreviewWindowClient(previewWindow_, streamResolution_);
    }
}

void HostController::BindControls(HWND statusLabel, HWND fpsCombo,
                                  HWND copyErrorButton) {
    statusLabel_ = statusLabel;
    fpsCombo_ = fpsCombo;
    copyErrorButton_ = copyErrorButton;
    SetStatus(kInitialStatus);
}

void HostController::WriteStatus(void* context, const wchar_t* text,
                                 StatusSeverity severity) {
    static_cast<HostController*>(context)->SetStatus(text, severity);
}

StatusSink HostController::StatusReporter() {
    return StatusSink{this, &HostController::WriteStatus};
}

void HostController::SetStatus(const wchar_t* text,
                               StatusSeverity severity,
                               const wchar_t* chinese) {
    if (!text) text = L"";
    wcsncpy_s(statusEnglish_, text, _TRUNCATE);
    if (chinese) {
        wcsncpy_s(statusChinese_, chinese, _TRUNCATE);
    } else {
        LocalizeDiagnostic(UiLanguage::SimplifiedChinese, text,
                           statusChinese_, _countof(statusChinese_));
    }
    statusSeverity_ = severity;
    RefreshStatusText();
}

void HostController::SetLanguage(UiLanguage language) {
    if (language != UiLanguage::English &&
        language != UiLanguage::SimplifiedChinese) return;
    language_ = language;
    RefreshStatusText();
    if (HasPreviewWindow() && !HasPreviewDevice()) {
        InvalidateRect(previewWindow_, nullptr, FALSE);
    }
}

void HostController::RefreshStatusText() {
    const wchar_t* text = language_ == UiLanguage::SimplifiedChinese
        ? statusChinese_ : statusEnglish_;
    const bool isError = statusSeverity_ == StatusSeverity::Error;
    if (isError) {
        wcsncpy_s(copyableError_,
                  sizeof(copyableError_) / sizeof(copyableError_[0]), text,
                  _TRUNCATE);
    } else {
        copyableError_[0] = L'\0';
    }
    if (copyErrorButton_) {
        SetWindowTextW(copyErrorButton_, UiText(language_, L"Copy error"));
        EnableWindow(copyErrorButton_, isError && copyableError_[0]);
    }
    if (statusLabel_) SetWindowTextW(statusLabel_, text);
}

void HostController::CopyErrorToClipboard() {
    if (shuttingDown_ || !copyErrorButton_ ||
        !IsWindow(copyErrorButton_) || !copyableError_[0]) {
        return;
    }
    const HWND owner = statusLabel_ ? GetAncestor(statusLabel_, GA_ROOT)
                                    : nullptr;
    const bool copied = clipboardWriter_ &&
        clipboardWriter_(owner, copyableError_);
    SetWindowTextW(copyErrorButton_,
                   UiText(language_, copied ? L"Copied!" : L"Copy failed"));
}

LONG HostController::SelectedFps() const {
    LONG fps = 30;
    const LRESULT selection =
        SendMessageW(fpsCombo_, CB_GETCURSEL, 0, 0);
    if (selection == 1) fps = 60;
    if (selection == 2) fps = 15;
    return fps;
}

void HostController::CloseSession() {
    session_.RequestStop();
    preview_.Reset();
    session_.Close();
}

void HostController::ClosePreviewWindow() {
    if (previewDestructionInProgress_) return;
    const HWND window = previewWindow_;
    if (!window || !IsWindow(window)) {
        preview_.Reset();
        preview_.SetOutputWindow(nullptr);
        previewWindow_ = nullptr;
        return;
    }

    previewDestructionInProgress_ = window;
    preview_.Reset();
    preview_.SetOutputWindow(nullptr);
    DestroyWindow(window);
    if (previewWindow_ == window) previewWindow_ = nullptr;
    previewDestructionInProgress_ = nullptr;
}

bool HostController::EnsurePreviewWindow() {
    if (HasPreviewWindow()) return true;
    preview_.Reset();
    preview_.SetOutputWindow(nullptr);
    previewWindow_ = nullptr;
    HWND window =
        CreatePreviewWindow(instance_, *this, streamResolution_);
    if (!window) return false;
    BindPreviewWindow(window);
    return true;
}

void HostController::StartCapture() {
    if (shuttingDown_) return;
    if (!EnsurePreviewWindow()) {
        SetStatus(L"The clean stream window could not be created.",
                  StatusSeverity::Error);
        return;
    }
    preview_.ResetFailureCount();
    const LONG fps = SelectedFps();

    FcsIpcV1* ipc = session_.Data();
    if (ipc && session_.TargetPid()) {
        if (!session_.TargetIsAlive()) {
            CloseSession();
        } else {
            if (ipc->state == FCS_STATE_HOOK_ORDER_LOST) {
                SetStatus(
                    L"MMOMinion changed its graphics hook after capture began. "
                    L"Restart FFXIV, wait until the MMOMinion GUI is visible, "
                    L"then click Start / Resume.", StatusSeverity::Error);
                return;
            }
            session_.Resume(fps);
            SetStatus(L"Resuming clean capture...");
            return;
        }
    }

    if (!session_.Data() && session_.TargetPid()) {
        session_.Close();
    }

    TargetInfo target{};
    if (!FindFfxiv(target)) {
        SetStatus(
            L"FFXIV (DirectX 11) was not found. Start the game and MMOMinion first.",
            StatusSeverity::Error);
        return;
    }
    if (!session_.Create(target.pid, target.window, fps)) {
        SetStatus(L"The private capture session could not be created.",
                  StatusSeverity::Error);
        CloseSession();
        return;
    }
    SetStatus(
        L"Attaching after MMOMinion... waiting for the clean game frame.");
    wchar_t error[256]{};
    if (!InjectHook(target.pid, error, 256)) {
        SetStatus(error, StatusSeverity::Error);
        CloseSession();
    }
}

void HostController::PauseCapture() {
    if (shuttingDown_) return;
    if (!HasPreviewWindow()) {
        SetStatus(
            L"The stream is ended. Click Start / Resume to open it again.");
        return;
    }
    if (session_.Pause()) {
        SetStatus(
            L"GPU frame copies are paused. Click Start / Resume to resume.");
    }
}

void HostController::EndCapture() {
    if (shuttingDown_) return;
    session_.RequestStop();
    ClosePreviewWindow();
    statusTicks_ = 0;
    if (!shuttingDown_) SetEndedStatus();
}

void HostController::SetEndedStatus() {
    SetStatus(
        L"Stream ended. The clean stream window is closed. Click Start / Resume to open it again.");
}

void HostController::UpdateStatus() {
    FcsIpcV1* ipc = session_.Data();
    if (!ipc) return;
    if (!session_.TargetIsAlive()) {
        CloseSession();
        SetStatus(
            L"FFXIV exited. Start the new game session, then click Start / Resume.");
        return;
    }
    if (preview_.FailureCount() >= 3) {
        SetStatus(
            L"The preview could not open this GPU format after three attempts. "
            L"Choose 30 FPS and click Start / Resume to try again.",
            StatusSeverity::Error);
        return;
    }
    wchar_t text[2][2048]{};
    wchar_t message[_countof(ipc->message)]{};
    wcsncpy_s(message, ipc->message, _countof(message) - 1);
    const LONG state = ipc->state;
    const LONG error = ipc->lastError;
    const LONG64 copies = ipc->copiesSubmitted;
    const LONG64 dropped = ipc->busyDropped;
    const LONG64 frequency = ipc->qpcFrequency;
    const double maxMicros = frequency > 0
        ? static_cast<double>(ipc->hookCpuTicksMax) * 1000000.0 /
              static_cast<double>(frequency)
        : 0.0;
    // Retain both renderings of the same snapshot. Changing language must not
    // restart capture, re-read a stale IPC state, or clear a current error.
    for (int index = 0; index < 2; ++index) {
        const UiLanguage language = index == 0
            ? UiLanguage::English : UiLanguage::SimplifiedChinese;
        wchar_t diagnostic[512]{};
        LocalizeDiagnostic(language, message, diagnostic, _countof(diagnostic));
        if (state == FCS_STATE_STREAMING) {
            swprintf_s(
                text[index], _countof(text[index]), UiText(language,
                L"Ready. Share the window named ‘FFXIV Clean Stream’ in Discord.\r\n\r\n"
                L"Clean frames: %lld    Busy frames dropped: %lld\r\n"
                L"Worst measured copy-submit CPU time: %.1f microseconds\r\n\r\n%s"),
                copies, dropped, maxMicros, diagnostic);
        } else if (state == FCS_STATE_WAITING_HOST_RESOURCES) {
            swprintf_s(
                text[index], _countof(text[index]), UiText(language,
                L"Preparing a compatibility GPU frame ring outside FFXIV...\r\n\r\n%s"),
                diagnostic);
        } else if (state == FCS_STATE_HOOK_ORDER_LOST) {
            swprintf_s(
                text[index], _countof(text[index]), UiText(language,
                L"Capture stopped safely because MMOMinion changed its graphics hook.\r\n\r\n"
                L"Restart FFXIV, wait until the MMOMinion GUI is visible, then click "
                L"Start / Resume.\r\n\r\n%s"),
                diagnostic);
        } else if (state == FCS_STATE_ERROR) {
            swprintf_s(text[index], _countof(text[index]),
                       UiText(language, L"Capture error (%ld):\r\n%s"),
                       error, diagnostic);
        } else {
            swprintf_s(text[index], _countof(text[index]), L"%s",
                       message[0] ? diagnostic : UiText(language, L"Waiting..."));
        }
    }
    SetStatus(text[0], state == FCS_STATE_ERROR ||
                        state == FCS_STATE_HOOK_ORDER_LOST
                    ? StatusSeverity::Error
                    : StatusSeverity::Info, text[1]);
}

void HostController::Tick() {
    if (shuttingDown_ || !HasPreviewWindow()) return;
    preview_.ServiceHostResourceRequest(session_, StatusReporter());
    preview_.ConsumeFrames(session_, StatusReporter());
    if (++statusTicks_ >= 30) {
        statusTicks_ = 0;
        UpdateStatus();
    }
}

void HostController::Shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    CloseSession();
    ClosePreviewWindow();
}

} // namespace fcs::host
