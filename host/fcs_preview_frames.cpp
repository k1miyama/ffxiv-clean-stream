#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "fcs_preview.hpp"

namespace fcs::host {

void PreviewRenderer::RequestResourceRecreate(CaptureSession& session,
                                              StatusSink status,
                                              const wchar_t* text) {
    const LONG64 failedGeneration = openGeneration_;
    // Ring/device changes are retried automatically and are not terminal
    // user-facing errors unless the controller reaches its failure limit.
    ReportStatus(status, text);
    if (session.Data() && ++viewerFailureCount_ <= 3) {
        session.RequestResourceRecreate();
    }
    Reset();
    openGeneration_ = failedGeneration;
}

bool PreviewRenderer::PollPlainConsumerAcks(CaptureSession& session,
                                            StatusSink status) {
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (!plainAckPending_[slot]) continue;
        const HRESULT hr = context_->GetData(
            plainConsumerQueries_[slot], nullptr, 0,
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) continue;
        if (FAILED(hr)) {
            RequestResourceRecreate(
                session, status,
                L"The preview GPU completion check failed; reconnecting.");
            return false;
        }
        if (!session.PublishPlainAck(slot, plainPendingSequence_[slot],
                                     plainPendingRingId_[slot])) {
            // Never acknowledge a frame from a retired generation. Destroying
            // the viewer clears the local pending state without touching IPC.
            RequestResourceRecreate(
                session, status,
                L"The compatibility frame ring changed; reconnecting.");
            return false;
        }
        plainAckPending_[slot] = false;
        plainPendingSequence_[slot] = 0;
        plainPendingRingId_[slot] = 0;
    }
    return true;
}

void PreviewRenderer::ConsumeFrames(CaptureSession& session,
                                    StatusSink status) {
    FcsIpcV1* ipc = session.Data();
    if (!ipc || ipc->controllerPid != GetCurrentProcessId() ||
        ipc->state != FCS_STATE_STREAMING ||
        !EnsureViewer(session, status)) {
        return;
    }
    // The producer can replace the ring immediately after EnsureViewer
    // returns. Branch only on the mode bound to the objects we actually hold;
    // consulting the live mode here could send an old plain viewer through
    // the keyed path (or vice versa) and dereference an absent interface.
    const FcsSharingMode boundMode = openSharingMode_;
    if (boundMode == FCS_SHARING_NONE ||
        ipc->generation != openGeneration_) {
        return;
    }
    const bool plainLegacy =
        boundMode == FCS_SHARING_LEGACY_PLAIN_HANDLE;
    if (plainLegacy &&
        ipc->resourceRequestId != openResourceRequestId_) {
        return;
    }
    if (plainLegacy && !PollPlainConsumerAcks(session, status)) return;
    LONG newestSlot = -1;
    LONG64 newestSequence = 0;
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        const LONG64 sequence = ipc->slotSequence[i];
        if (plainLegacy && sequence > consumedSequence_[i] &&
            sequence <= lastPlainPreviewSequence_) {
            // A defensive monotonicity guard for drivers that report event
            // queries out of order: retire, but never display, the old frame.
            if (!session.PublishPlainAck(
                    i, sequence, openResourceRequestId_)) {
                RequestResourceRecreate(
                    session, status,
                    L"The compatibility frame ring changed; reconnecting.");
                return;
            }
            consumedSequence_[i] = sequence;
            continue;
        }
        if (sequence > consumedSequence_[i] && sequence > newestSequence) {
            newestSequence = sequence;
            newestSlot = static_cast<LONG>(i);
        }
    }
    if (newestSlot < 0) return;

    // Return stale ring slots first so a slow/minimized preview cannot block
    // the game.
    for (UINT i = 0; i < FCS_SLOT_COUNT; ++i) {
        const LONG64 sequence = ipc->slotSequence[i];
        if (static_cast<LONG>(i) == newestSlot ||
            sequence <= consumedSequence_[i]) {
            continue;
        }
        if (plainLegacy) {
            // This slot was never read by the consumer GPU, so it is safe to
            // return immediately without a completion query.
            if (!session.PublishPlainAck(
                    i, sequence, openResourceRequestId_)) {
                RequestResourceRecreate(
                    session, status,
                    L"The compatibility frame ring changed; reconnecting.");
                return;
            }
            consumedSequence_[i] = sequence;
            continue;
        }
        const HRESULT acquire = keyedMutexes_[i]->AcquireSync(1, 0);
        if (acquire == S_OK) {
            consumedSequence_[i] = sequence;
            if (keyedMutexes_[i]->ReleaseSync(0) != S_OK) {
                RequestResourceRecreate(
                    session, status,
                    L"The GPU frame ring lost synchronization; reconnecting.");
                return;
            }
        } else if (acquire != static_cast<HRESULT>(WAIT_TIMEOUT)) {
            RequestResourceRecreate(
                session, status,
                L"The GPU frame ring was abandoned; reconnecting.");
            return;
        }
    }

    if (plainLegacy) {
        if (plainAckPending_[newestSlot]) return;
        context_->CopyResource(previewBuffer_, sharedTextures_[newestSlot]);
        context_->End(plainConsumerQueries_[newestSlot]);
        plainPendingSequence_[newestSlot] = newestSequence;
        plainPendingRingId_[newestSlot] = openResourceRequestId_;
        plainAckPending_[newestSlot] = true;
        consumedSequence_[newestSlot] = newestSequence;
        lastPlainPreviewSequence_ = newestSequence;
    } else {
        const HRESULT acquire =
            keyedMutexes_[newestSlot]->AcquireSync(1, 0);
        if (acquire == static_cast<HRESULT>(WAIT_TIMEOUT)) return;
        if (acquire != S_OK) {
            RequestResourceRecreate(
                session, status,
                L"The GPU frame ring was abandoned; reconnecting.");
            return;
        }
        context_->CopyResource(previewBuffer_, sharedTextures_[newestSlot]);
        consumedSequence_[newestSlot] = newestSequence;
        if (keyedMutexes_[newestSlot]->ReleaseSync(0) != S_OK) {
            RequestResourceRecreate(
                session, status,
                L"The GPU frame ring lost synchronization; reconnecting.");
            return;
        }
    }
    const HRESULT present = previewSwap_->Present(0, 0);
    if (FAILED(present)) {
        RequestResourceRecreate(
            session, status,
            present == DXGI_ERROR_DEVICE_REMOVED ||
                    present == DXGI_ERROR_DEVICE_RESET
                ? L"The preview GPU device was reset; reconnecting."
                : L"The clean preview could not present a frame; reconnecting.");
    }
}

} // namespace fcs::host
