#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgiformat.h>
#include <stdint.h>

#include "../common/fcs_ipc.hpp"

namespace fcs::host {

struct StableMetadata {
    LONG64 generation;
    wchar_t names[FCS_SLOT_COUNT][96];
    uint64_t handles[FCS_SLOT_COUNT];
    FcsSharingMode sharingMode;
    uint64_t adapterLuid;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LONG64 resourceRequestId;
    FcsSharingMode requestedHostMode;
};

struct HostResourceResponse {
    LONG64 requestId;
    FcsSharingMode sharingMode;
    HRESULT error;
    FcsResourceStage failureStage;
    wchar_t names[FCS_SLOT_COUNT][96];
    uint64_t handles[FCS_SLOT_COUNT];
};

class CaptureSession final {
public:
    CaptureSession() = default;
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    bool Create(DWORD pid, HWND target, LONG fps);
    void RequestStop();
    void Resume(LONG fps);
    bool Pause();
    void Close();

    bool TargetIsAlive() const;
    bool ReadStableMetadata(StableMetadata& out) const;
    bool PublishHostResponse(const HostResourceResponse& response);
    bool PublishPlainAck(UINT slot, LONG64 sequence, LONG64 ringId);
    void RequestResourceRecreate();

    FcsIpcV1* Data() const { return ipc_; }
    DWORD TargetPid() const { return targetPid_; }
    HWND TargetWindow() const { return targetWindow_; }

private:
    void BeginHostResponseWrite();
    void EndHostResponseWrite();

    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    HANDLE targetProcess_ = nullptr;
    FcsIpcV1* ipc_ = nullptr;
    DWORD targetPid_ = 0;
    HWND targetWindow_ = nullptr;
};

} // namespace fcs::host
