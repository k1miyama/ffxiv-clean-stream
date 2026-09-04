#pragma once

#include <windows.h>
#include <stdint.h>

static constexpr uint32_t FCS_MAGIC = 0x35534346u; // "FCS5"
static constexpr uint16_t FCS_VERSION = 5;
static constexpr uint32_t FCS_SLOT_COUNT = 3;

enum FcsSharingMode : uint32_t {
    FCS_SHARING_NONE = 0,
    FCS_SHARING_NT_NAME = 1,
    FCS_SHARING_LEGACY_HANDLE = 2,
    // The controller owns the named keyed-mutex textures. This avoids asking
    // an intercepted game device to create shared resources; the game device
    // only opens them and submits nonblocking copies.
    FCS_SHARING_HOST_NT_NAME = 3,
    FCS_SHARING_HOST_REQUEST = 4,
    // The controller owns legacy keyed-mutex textures. Their opaque DXGI
    // handles are valid in both processes and are opened with
    // ID3D11Device::OpenSharedResource.
    FCS_SHARING_HOST_LEGACY_HANDLE = 5,
    // The producer owns ordinary legacy shared textures without keyed
    // mutexes. GPU event queries plus the per-slot consumer acknowledgement
    // fields below guarantee that a slot is never reused while either GPU is
    // still touching it.
    FCS_SHARING_LEGACY_PLAIN_HANDLE = 6,
};

enum FcsState : LONG {
    FCS_STATE_IDLE = 0,
    FCS_STATE_INJECTED = 1,
    FCS_STATE_HOOKED = 2,
    FCS_STATE_STREAMING = 3,
    FCS_STATE_PAUSED = 4,
    FCS_STATE_WAITING_HOST_RESOURCES = 5,
    FCS_STATE_ERROR = -1,
    FCS_STATE_HOOK_ORDER_LOST = -2,
};

enum FcsCommand : LONG {
    FCS_COMMAND_NONE = 0,
    // Kept reserved so existing command bits remain stable. The
    // helper deliberately never rewraps a live swap chain: doing so can place
    // the same third-party overlay in the Present chain more than once.
    FCS_COMMAND_RESERVED_REWRAP = 1,
    FCS_COMMAND_RECREATE_RESOURCES = 2,
    // Diagnostic/test escape hatch. Normal sessions first try resources owned
    // by the game process and set this only when explicitly exercising the
    // reverse-ownership path.
    FCS_COMMAND_FORCE_HOST_RESOURCES = 4,
    // Test-only fault injection: request host NT resources normally, then
    // behave as if the game device rejected both ways of opening them. This
    // exercises the automatic host-legacy fallback end to end.
    FCS_COMMAND_TEST_REJECT_HOST_NT = 8,
    // Test-only fault injection for the intermediate compatibility path:
    // reject the name open but allow DuplicateHandle + OpenSharedResource1.
    FCS_COMMAND_TEST_REJECT_HOST_NT_NAME = 16,
    // Test-only fault injection: reject the controller-owned legacy keyed
    // ring so the producer-created plain legacy fallback is exercised.
    FCS_COMMAND_TEST_REJECT_HOST_LEGACY = 32,
};

enum FcsResourceStage : uint32_t {
    FCS_RESOURCE_STAGE_NONE = 0,
    FCS_RESOURCE_STAGE_CREATE_TEXTURE = 1,
    FCS_RESOURCE_STAGE_KEYED_MUTEX = 2,
    FCS_RESOURCE_STAGE_DXGI_RESOURCE = 3,
    FCS_RESOURCE_STAGE_CREATE_HANDLE = 4,
    FCS_RESOURCE_STAGE_FIND_ADAPTER = 5,
    FCS_RESOURCE_STAGE_CREATE_DEVICE = 6,
    FCS_RESOURCE_STAGE_GET_PREVIEW_BUFFER = 7,
    FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME = 8,
    FCS_RESOURCE_STAGE_DUPLICATE_NT_HANDLE = 9,
    FCS_RESOURCE_STAGE_OPEN_SHARED_NT_HANDLE = 10,
    FCS_RESOURCE_STAGE_OPEN_SHARED_LEGACY = 11,
    FCS_RESOURCE_STAGE_CREATE_QUERY = 12,
    FCS_RESOURCE_STAGE_QUERY_COMPLETION = 13,
};

// This structure lives in a named page-file mapping shared by the controller
// and the injected helper. Only fixed-width values cross the process boundary.
struct alignas(64) FcsIpcV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint32_t producerPid;
    uint32_t controllerPid;
    volatile LONG state;
    volatile LONG command;
    volatile LONG captureFps;
    volatile LONG lastError;
    volatile LONG stopRequested;
    volatile LONG latestSlot;
    volatile LONG64 generation; // seqlock: odd while metadata is changing
    volatile LONG64 frameId;
    volatile LONG64 slotSequence[FCS_SLOT_COUNT];
    // Written only by the controller. In plain legacy mode the producer may
    // reuse a published slot only after the sequence matches that exact frame
    // and slotAckRingId matches the ring's resourceRequestId. The latter
    // is a stable resource-generation token, unlike the metadata seqlock.
    // Keyed-mutex modes ignore these fields.
    volatile LONG64 slotAckSequence[FCS_SLOT_COUNT];
    volatile LONG64 slotAckRingId[FCS_SLOT_COUNT];
    wchar_t sharedNames[FCS_SLOT_COUNT][96];
    // Legacy DXGI shared handles are opaque, non-NT values. They are copied
    // verbatim to the consumer and must not be passed to CloseHandle.
    uint64_t sharedHandles[FCS_SLOT_COUNT];
    uint32_t sharingMode;
    uint32_t producerNtStage;
    uint32_t producerLegacyStage;
    volatile LONG producerNtError;
    volatile LONG producerLegacyError;
    uint32_t producerPlainStage;
    volatile LONG producerPlainError;
    // These path-diagnostic fields are written only by the producer, under generation.
    // The controller's current response stays under hostGeneration below.
    uint32_t hostNtNamePathStage;
    uint32_t hostNtHandlePathStage;
    uint32_t hostLegacyPathStage;
    volatile LONG hostNtNamePathError;
    volatile LONG hostNtHandlePathError;
    volatile LONG hostLegacyPathError;
    // Producer-owned request token. A new value means the previous host-owned
    // ring is no longer in use by the game and may be released by the host.
    volatile LONG64 resourceRequestId;
    uint32_t hostRequestedSharingMode;
    // Host-owned response packet with an independent seqlock. Keeping this
    // separate from generation means the two processes never share a writer.
    volatile LONG64 hostGeneration;
    volatile LONG64 hostRequestId;
    uint32_t hostControllerPid;
    volatile LONG hostResourceError;
    uint32_t hostFailureStage;
    uint32_t hostResponseSharingMode;
    wchar_t hostSharedNames[FCS_SLOT_COUNT][96];
    uint64_t hostSharedHandles[FCS_SLOT_COUNT];
    uint64_t adapterLuid;
    uint64_t targetHwnd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t sampleCount;
    volatile LONG64 presentCount;
    volatile LONG64 bootstrapPresentCount;
    volatile LONG64 targetRejectedPresentCount;
    volatile LONG64 copiesSubmitted;
    volatile LONG64 rateSkipped;
    volatile LONG64 busyDropped;
    volatile LONG64 rewrapCount;
    volatile LONG64 qpcLastFrame;
    volatile LONG64 qpcFrequency;
    volatile LONG64 hookCpuTicksMax;
    wchar_t message[256];
    uint8_t reserved[512];
};

static_assert(sizeof(FcsIpcV1) < 4096, "IPC structure must fit in one mapping page");

inline void FcsMappingName(wchar_t* output, DWORD pid) {
    wsprintfW(output, L"Local\\FCS5.Control.%lu", static_cast<unsigned long>(pid));
}

inline void FcsReadyEventName(wchar_t* output, DWORD pid) {
    wsprintfW(output, L"Local\\FCS5.Ready.%lu", static_cast<unsigned long>(pid));
}

inline void FcsFrameEventName(wchar_t* output, DWORD pid) {
    wsprintfW(output, L"Local\\FCS5.Frame.%lu", static_cast<unsigned long>(pid));
}
