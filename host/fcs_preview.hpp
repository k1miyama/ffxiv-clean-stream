#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "../common/fcs_ipc.hpp"
#include "fcs_session.hpp"
#include "fcs_status.hpp"

namespace fcs::host {

class PreviewRenderer final {
public:
    PreviewRenderer() = default;
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    void SetOutputWindow(HWND window) { outputWindow_ = window; }
    void Reset();
    void ResetFailureCount() { viewerFailureCount_ = 0; }

    bool HasDevice() const { return device_ != nullptr; }
    LONG FailureCount() const { return viewerFailureCount_; }

    void ServiceHostResourceRequest(CaptureSession& session, StatusSink status);
    void ConsumeFrames(CaptureSession& session, StatusSink status);

private:
    bool SetupFailed(CaptureSession& session, const StableMetadata& metadata,
                     StatusSink status, const wchar_t* text);
    bool CreateHostOwnedViewer(CaptureSession& session,
                               const StableMetadata& metadata,
                               StatusSink status);
    bool CreateViewerForMetadata(CaptureSession& session,
                                 const StableMetadata& metadata,
                                 StatusSink status);
    bool EnsureViewer(CaptureSession& session, StatusSink status);
    void RequestResourceRecreate(CaptureSession& session, StatusSink status,
                                 const wchar_t* text);
    bool PollPlainConsumerAcks(CaptureSession& session, StatusSink status);

    HWND outputWindow_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11Device1* device1_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* previewSwap_ = nullptr;
    ID3D11Texture2D* previewBuffer_ = nullptr;
    ID3D11Texture2D* sharedTextures_[FCS_SLOT_COUNT]{};
    IDXGIKeyedMutex* keyedMutexes_[FCS_SLOT_COUNT]{};
    HANDLE sharedNtHandles_[FCS_SLOT_COUNT]{};
    ID3D11Query* plainConsumerQueries_[FCS_SLOT_COUNT]{};
    bool plainAckPending_[FCS_SLOT_COUNT]{};
    LONG64 plainPendingSequence_[FCS_SLOT_COUNT]{};
    LONG64 plainPendingRingId_[FCS_SLOT_COUNT]{};
    LONG64 openGeneration_ = -1;
    LONG64 openResourceRequestId_ = 0;
    FcsSharingMode openSharingMode_ = FCS_SHARING_NONE;
    LONG64 consumedSequence_[FCS_SLOT_COUNT]{};
    LONG64 lastPlainPreviewSequence_ = 0;
    LONG viewerFailureCount_ = 0;
    LONG64 ownedHostRequestId_ = 0;
    FcsSharingMode ownedHostSharingMode_ = FCS_SHARING_NONE;
    LONG64 failedHostRequestId_ = 0;
};

} // namespace fcs::host
