#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fcs_session.hpp"

namespace fcs::host {

CaptureSession::~CaptureSession() {
    Close();
}

bool CaptureSession::Create(DWORD pid, HWND target, LONG fps) {
    targetPid_ = pid;
    targetWindow_ = target;
    targetProcess_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!targetProcess_) return false;

    wchar_t name[96]{};
    FcsMappingName(name, pid);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, sizeof(FcsIpcV1), name);
    if (!mapping_) return false;
    const bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
    ipc_ = static_cast<FcsIpcV1*>(MapViewOfFile(
        mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FcsIpcV1)));
    if (!ipc_) return false;
    if (!existing || ipc_->magic != FCS_MAGIC ||
        ipc_->version != FCS_VERSION || ipc_->bytes != sizeof(FcsIpcV1)) {
        ZeroMemory(ipc_, sizeof(FcsIpcV1));
        ipc_->magic = FCS_MAGIC;
        ipc_->version = FCS_VERSION;
        ipc_->bytes = sizeof(FcsIpcV1);
        ipc_->latestSlot = -1;
    }
    ipc_->controllerPid = GetCurrentProcessId();
    ipc_->targetHwnd = reinterpret_cast<uint64_t>(target);
    InterlockedExchange(&ipc_->captureFps, fps);
    InterlockedExchange(&ipc_->stopRequested, 0);
    InterlockedOr(&ipc_->command, FCS_COMMAND_RECREATE_RESOURCES);

    FcsReadyEventName(name, pid);
    readyEvent_ = CreateEventW(nullptr, FALSE, FALSE, name);
    FcsFrameEventName(name, pid);
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, name);
    return readyEvent_ && frameEvent_;
}

void CaptureSession::RequestStop() {
    if (ipc_ && ipc_->controllerPid == GetCurrentProcessId()) {
        InterlockedExchange(&ipc_->stopRequested, 1);
    }
}

void CaptureSession::Resume(LONG fps) {
    if (!ipc_) return;
    ipc_->controllerPid = GetCurrentProcessId();
    InterlockedExchange(&ipc_->captureFps, fps);
    InterlockedExchange(&ipc_->stopRequested, 0);
    InterlockedOr(&ipc_->command, FCS_COMMAND_RECREATE_RESOURCES);
}

bool CaptureSession::Pause() {
    if (!ipc_ || ipc_->controllerPid != GetCurrentProcessId()) return false;
    InterlockedExchange(&ipc_->stopRequested, 1);
    return true;
}

void CaptureSession::Close() {
    if (ipc_) {
        UnmapViewOfFile(ipc_);
        ipc_ = nullptr;
    }
    if (frameEvent_) CloseHandle(frameEvent_);
    if (readyEvent_) CloseHandle(readyEvent_);
    if (mapping_) CloseHandle(mapping_);
    if (targetProcess_) CloseHandle(targetProcess_);
    frameEvent_ = nullptr;
    readyEvent_ = nullptr;
    mapping_ = nullptr;
    targetProcess_ = nullptr;
    targetPid_ = 0;
    targetWindow_ = nullptr;
}

bool CaptureSession::TargetIsAlive() const {
    if (!targetProcess_ ||
        WaitForSingleObject(targetProcess_, 0) != WAIT_TIMEOUT ||
        !IsWindow(targetWindow_)) {
        return false;
    }
    DWORD owner = 0;
    GetWindowThreadProcessId(targetWindow_, &owner);
    return owner == targetPid_;
}

bool CaptureSession::ReadStableMetadata(StableMetadata& out) const {
    if (!ipc_ || ipc_->magic != FCS_MAGIC ||
        ipc_->version != FCS_VERSION || ipc_->bytes != sizeof(FcsIpcV1)) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG64 before = ipc_->generation;
        MemoryBarrier();
        if (before & 1) continue;
        out.generation = before;
        out.adapterLuid = ipc_->adapterLuid;
        out.width = ipc_->width;
        out.height = ipc_->height;
        out.format = static_cast<DXGI_FORMAT>(ipc_->format);
        out.sharingMode = static_cast<FcsSharingMode>(ipc_->sharingMode);
        out.resourceRequestId = ipc_->resourceRequestId;
        out.requestedHostMode =
            static_cast<FcsSharingMode>(ipc_->hostRequestedSharingMode);
        for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
            lstrcpynW(out.names[i], ipc_->sharedNames[i], 96);
            out.handles[i] = ipc_->sharedHandles[i];
        }
        MemoryBarrier();
        const LONG64 after = ipc_->generation;
        if (before == after && !(after & 1)) {
            if (!out.width || !out.height ||
                out.format == DXGI_FORMAT_UNKNOWN) {
                return false;
            }
            for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
                if ((out.sharingMode == FCS_SHARING_NT_NAME ||
                     out.sharingMode == FCS_SHARING_HOST_NT_NAME) &&
                    !out.names[i][0]) {
                    return false;
                }
                if ((out.sharingMode == FCS_SHARING_LEGACY_HANDLE ||
                     out.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE ||
                     out.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE) &&
                    !out.handles[i]) {
                    return false;
                }
            }
            if (out.sharingMode != FCS_SHARING_NT_NAME &&
                out.sharingMode != FCS_SHARING_LEGACY_HANDLE &&
                out.sharingMode != FCS_SHARING_HOST_NT_NAME &&
                out.sharingMode != FCS_SHARING_HOST_LEGACY_HANDLE &&
                out.sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE &&
                out.sharingMode != FCS_SHARING_HOST_REQUEST) {
                return false;
            }
            if ((out.sharingMode == FCS_SHARING_HOST_REQUEST ||
                 out.sharingMode == FCS_SHARING_HOST_NT_NAME ||
                 out.sharingMode == FCS_SHARING_HOST_LEGACY_HANDLE ||
                 out.sharingMode == FCS_SHARING_LEGACY_PLAIN_HANDLE) &&
                !out.resourceRequestId) {
                return false;
            }
            if (out.sharingMode == FCS_SHARING_HOST_REQUEST &&
                out.requestedHostMode != FCS_SHARING_HOST_NT_NAME &&
                out.requestedHostMode != FCS_SHARING_HOST_LEGACY_HANDLE) {
                return false;
            }
            return true;
        }
    }
    return false;
}

void CaptureSession::BeginHostResponseWrite() {
    LONG64 generation = InterlockedIncrement64(&ipc_->hostGeneration);
    if ((generation & 1) == 0) {
        InterlockedIncrement64(&ipc_->hostGeneration);
    }
    MemoryBarrier();
}

void CaptureSession::EndHostResponseWrite() {
    MemoryBarrier();
    LONG64 generation = InterlockedIncrement64(&ipc_->hostGeneration);
    if ((generation & 1) != 0) {
        InterlockedIncrement64(&ipc_->hostGeneration);
    }
    if (readyEvent_) SetEvent(readyEvent_);
}

bool CaptureSession::PublishHostResponse(
    const HostResourceResponse& response) {
    if (!ipc_ || ipc_->controllerPid != GetCurrentProcessId() ||
        ipc_->resourceRequestId != response.requestId) {
        return false;
    }
    BeginHostResponseWrite();
    ipc_->hostRequestId = response.requestId;
    ipc_->hostControllerPid = GetCurrentProcessId();
    ipc_->hostResourceError = static_cast<LONG>(response.error);
    ipc_->hostFailureStage = response.failureStage;
    ipc_->hostResponseSharingMode = response.sharingMode;
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        lstrcpynW(ipc_->hostSharedNames[slot], response.names[slot], 96);
        ipc_->hostSharedHandles[slot] = response.handles[slot];
    }
    EndHostResponseWrite();
    return true;
}

bool CaptureSession::PublishPlainAck(UINT slot, LONG64 sequence,
                                     LONG64 ringId) {
    if (slot >= FCS_SLOT_COUNT || !ipc_ ||
        ipc_->controllerPid != GetCurrentProcessId() ||
        ipc_->resourceRequestId != ringId ||
        ipc_->sharingMode != FCS_SHARING_LEGACY_PLAIN_HANDLE ||
        ipc_->slotSequence[slot] != sequence) {
        return false;
    }
    // Sequence is the commit field. The producer samples it on both sides of
    // the stable ring-ID read before accepting the acknowledgement.
    InterlockedExchange64(&ipc_->slotAckRingId[slot], ringId);
    MemoryBarrier();
    InterlockedExchange64(&ipc_->slotAckSequence[slot], sequence);
    return true;
}

void CaptureSession::RequestResourceRecreate() {
    if (ipc_) {
        InterlockedOr(&ipc_->command, FCS_COMMAND_RECREATE_RESOURCES);
    }
}

} // namespace fcs::host
