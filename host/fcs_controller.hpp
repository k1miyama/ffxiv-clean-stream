#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fcs_localization.hpp"
#include "fcs_preview.hpp"
#include "fcs_session.hpp"
#include "fcs_status.hpp"
#include "fcs_stream_resolution.hpp"

namespace fcs::host {

using ClipboardWriter = bool (*)(HWND owner, const wchar_t* text);

class HostController final {
public:
    explicit HostController(HINSTANCE instance);
    HostController(HINSTANCE instance, ClipboardWriter clipboardWriter);
    ~HostController();

    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    void BindPreviewWindow(HWND window);
    void BindControls(HWND statusLabel, HWND fpsCombo,
                      HWND copyErrorButton);
    void OnPreviewWindowDestroyed(HWND window);

    void StartCapture();
    void PauseCapture();
    void EndCapture();
    void CopyErrorToClipboard();
    void SetLanguage(UiLanguage language);
    UiLanguage Language() const { return language_; }
    void Tick();
    void Shutdown();

    HWND PreviewWindow() const { return previewWindow_; }
    bool HasPreviewWindow() const;
    void SetStreamResolution(StreamResolution resolution);
    StreamResolution SelectedStreamResolution() const {
        return streamResolution_;
    }
    bool HasPreviewDevice() const { return preview_.HasDevice(); }

private:
    static void WriteStatus(void* context, const wchar_t* text,
                            StatusSeverity severity);
    StatusSink StatusReporter();
    void SetStatus(const wchar_t* text,
                   StatusSeverity severity = StatusSeverity::Info,
                   const wchar_t* chinese = nullptr);
    void RefreshStatusText();
    void CloseSession();
    void ClosePreviewWindow();
    bool EnsurePreviewWindow();
    void SetEndedStatus();
    void UpdateStatus();
    LONG SelectedFps() const;

    HINSTANCE instance_ = nullptr;
    ClipboardWriter clipboardWriter_ = nullptr;
    CaptureSession session_;
    PreviewRenderer preview_;
    HWND previewWindow_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND fpsCombo_ = nullptr;
    HWND copyErrorButton_ = nullptr;
    wchar_t copyableError_[2048]{};
    wchar_t statusEnglish_[2048]{};
    wchar_t statusChinese_[2048]{};
    StatusSeverity statusSeverity_ = StatusSeverity::Info;
    UiLanguage language_ = UiLanguage::English;
    StreamResolution streamResolution_ = kDefaultStreamResolution;
    UINT statusTicks_ = 0;
    HWND previewDestructionInProgress_ = nullptr;
    bool shuttingDown_ = false;
};

} // namespace fcs::host
