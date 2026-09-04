#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wchar.h>

#include "fcs_controller.hpp"
#include "fcs_preview_window.hpp"
#include "fcs_target.hpp"

namespace fcs::host {

HostController::HostController(HINSTANCE instance)
    : instance_(instance ? instance : GetModuleHandleW(nullptr)) {}

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

void HostController::BindControls(HWND statusLabel, HWND fpsCombo) {
    statusLabel_ = statusLabel;
    fpsCombo_ = fpsCombo;
}

void HostController::WriteStatus(void* context, const wchar_t* text) {
    static_cast<HostController*>(context)->SetStatus(text);
}

StatusSink HostController::StatusReporter() {
    return StatusSink{this, &HostController::WriteStatus};
}

void HostController::SetStatus(const wchar_t* text) {
    if (statusLabel_) SetWindowTextW(statusLabel_, text);
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
        SetStatus(L"The clean stream window could not be created.");
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
                    L"then click Start / Resume.");
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
            L"FFXIV (DirectX 11) was not found. Start the game and MMOMinion first.");
        return;
    }
    if (!session_.Create(target.pid, target.window, fps)) {
        SetStatus(L"The private capture session could not be created.");
        CloseSession();
        return;
    }
    SetStatus(
        L"Attaching after MMOMinion... waiting for the clean game frame.");
    wchar_t error[256]{};
    if (!InjectHook(target.pid, error, 256)) {
        SetStatus(error);
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
            L"Choose 30 FPS and click Start / Resume to try again.");
        return;
    }
    wchar_t text[768]{};
    const LONG state = ipc->state;
    if (state == FCS_STATE_STREAMING) {
        const double maxMicros = ipc->qpcFrequency > 0
            ? (static_cast<double>(ipc->hookCpuTicksMax) * 1000000.0 /
               static_cast<double>(ipc->qpcFrequency))
            : 0.0;
        swprintf_s(
            text, 768,
            L"Ready. Share the window named ‘FFXIV Clean Stream’ in Discord.\r\n\r\n"
            L"Clean frames: %lld    Busy frames dropped: %lld\r\n"
            L"Worst measured copy-submit CPU time: %.1f microseconds\r\n\r\n%s",
            ipc->copiesSubmitted, ipc->busyDropped, maxMicros, ipc->message);
    } else if (state == FCS_STATE_WAITING_HOST_RESOURCES) {
        swprintf_s(
            text, 768,
            L"Preparing a compatibility GPU frame ring outside FFXIV...\r\n\r\n%s",
            ipc->message);
    } else if (state == FCS_STATE_HOOK_ORDER_LOST) {
        swprintf_s(
            text, 768,
            L"Capture stopped safely because MMOMinion changed its graphics hook.\r\n\r\n"
            L"Restart FFXIV, wait until the MMOMinion GUI is visible, then click "
            L"Start / Resume.\r\n\r\n%s",
            ipc->message);
    } else if (state == FCS_STATE_ERROR) {
        swprintf_s(text, 768, L"Capture error (%ld):\r\n%s",
                   ipc->lastError, ipc->message);
    } else {
        swprintf_s(text, 768, L"%s",
                   ipc->message[0] ? ipc->message : L"Waiting...");
    }
    SetStatus(text);
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
