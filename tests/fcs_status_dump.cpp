#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "../common/fcs_ipc.hpp"

namespace {

const wchar_t* StateName(LONG state) {
    switch (state) {
        case FCS_STATE_IDLE: return L"idle";
        case FCS_STATE_INJECTED: return L"injected";
        case FCS_STATE_HOOKED: return L"hooked";
        case FCS_STATE_STREAMING: return L"streaming";
        case FCS_STATE_PAUSED: return L"paused";
        case FCS_STATE_WAITING_HOST_RESOURCES: return L"waiting-host-resources";
        case FCS_STATE_ERROR: return L"error";
        case FCS_STATE_HOOK_ORDER_LOST: return L"hook-order-lost";
        default: return L"unknown";
    }
}

const wchar_t* FormatName(uint32_t format) {
    switch (format) {
        case 0: return L"DXGI_FORMAT_UNKNOWN";
        case 10: return L"DXGI_FORMAT_R16G16B16A16_FLOAT";
        case 24: return L"DXGI_FORMAT_R10G10B10A2_UNORM";
        case 28: return L"DXGI_FORMAT_R8G8B8A8_UNORM";
        case 29: return L"DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case 87: return L"DXGI_FORMAT_B8G8R8A8_UNORM";
        case 88: return L"DXGI_FORMAT_B8G8R8X8_UNORM";
        case 91: return L"DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case 93: return L"DXGI_FORMAT_B8G8R8X8_UNORM_SRGB";
        default: return L"unlisted DXGI format";
    }
}

const wchar_t* SharingModeName(uint32_t mode) {
    switch (mode) {
        case FCS_SHARING_NONE: return L"none";
        case FCS_SHARING_NT_NAME: return L"NT named handle";
        case FCS_SHARING_LEGACY_HANDLE: return L"legacy DXGI handle";
        case FCS_SHARING_HOST_NT_NAME: return L"host-owned NT named handle";
        case FCS_SHARING_HOST_REQUEST: return L"host-owned ring requested";
        case FCS_SHARING_HOST_LEGACY_HANDLE: return L"host-owned legacy DXGI handle";
        case FCS_SHARING_LEGACY_PLAIN_HANDLE: return L"plain legacy DXGI handle";
        default: return L"unknown";
    }
}

const wchar_t* ResourceStageName(uint32_t stage) {
    switch (stage) {
        case FCS_RESOURCE_STAGE_NONE: return L"none";
        case FCS_RESOURCE_STAGE_CREATE_TEXTURE: return L"CreateTexture2D";
        case FCS_RESOURCE_STAGE_KEYED_MUTEX: return L"IDXGIKeyedMutex";
        case FCS_RESOURCE_STAGE_DXGI_RESOURCE: return L"IDXGIResource";
        case FCS_RESOURCE_STAGE_CREATE_HANDLE: return L"Create/GetSharedHandle";
        case FCS_RESOURCE_STAGE_FIND_ADAPTER: return L"find adapter";
        case FCS_RESOURCE_STAGE_CREATE_DEVICE: return L"create host device";
        case FCS_RESOURCE_STAGE_GET_PREVIEW_BUFFER: return L"get preview buffer";
        case FCS_RESOURCE_STAGE_OPEN_SHARED_NT_NAME: return L"OpenSharedResourceByName";
        case FCS_RESOURCE_STAGE_DUPLICATE_NT_HANDLE: return L"DuplicateHandle";
        case FCS_RESOURCE_STAGE_OPEN_SHARED_NT_HANDLE: return L"OpenSharedResource1";
        case FCS_RESOURCE_STAGE_OPEN_SHARED_LEGACY: return L"OpenSharedResource";
        case FCS_RESOURCE_STAGE_CREATE_QUERY: return L"CreateQuery";
        case FCS_RESOURCE_STAGE_QUERY_COMPLETION: return L"query completion";
        default: return L"unknown";
    }
}

bool IsFfxivDx11(const wchar_t* name) {
    return name != nullptr && _wcsicmp(name, L"ffxiv_dx11.exe") == 0;
}

void CopyMessage(wchar_t (&destination)[256], const volatile wchar_t* source) {
    size_t index = 0;
    for (; index + 1 < (sizeof(destination) / sizeof(destination[0])); ++index) {
        const wchar_t character = source[index];
        destination[index] = character;
        if (character == L'\0') {
            return;
        }
    }
    destination[index] = L'\0';
}

bool DumpForPid(DWORD pid) {
    wchar_t mappingName[64] = {};
    FcsMappingName(mappingName, pid);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
    if (mapping == nullptr) {
        wprintf(L"ffxiv_dx11.exe PID %lu: no Clean Stream mapping (%ls); error=%lu\n",
                static_cast<unsigned long>(pid), mappingName,
                static_cast<unsigned long>(GetLastError()));
        return false;
    }

    const auto* ipc = static_cast<const FcsIpcV1*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(FcsIpcV1)));
    if (ipc == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        wprintf(L"ffxiv_dx11.exe PID %lu: mapping exists but cannot be viewed; error=%lu\n",
                static_cast<unsigned long>(pid), static_cast<unsigned long>(error));
        return false;
    }

    wchar_t message[256] = {};
    CopyMessage(message, ipc->message);

    const LONG state = ipc->state;
    const LONG lastError = ipc->lastError;
    const uint32_t format = ipc->format;

    wprintf(L"Clean Stream status for ffxiv_dx11.exe PID %lu\n",
            static_cast<unsigned long>(pid));
    wprintf(L"  mapping:          %ls\n", mappingName);
    wprintf(L"  protocol:         magic=0x%08lX version=%u bytes=%u\n",
            static_cast<unsigned long>(ipc->magic),
            static_cast<unsigned int>(ipc->version),
            static_cast<unsigned int>(ipc->bytes));
    wprintf(L"  producer PID:     %lu\n", static_cast<unsigned long>(ipc->producerPid));
    wprintf(L"  controller PID:   %lu\n", static_cast<unsigned long>(ipc->controllerPid));
    wprintf(L"  state:            %ld (%ls)\n", static_cast<long>(state), StateName(state));
    wprintf(L"  command:          %ld\n", static_cast<long>(ipc->command));
    wprintf(L"  capture FPS:      %ld\n", static_cast<long>(ipc->captureFps));
    wprintf(L"  last error:       %ld (0x%08lX)\n",
            static_cast<long>(lastError),
            static_cast<unsigned long>(static_cast<uint32_t>(lastError)));
    wprintf(L"  message:          %ls\n", message[0] != L'\0' ? message : L"(empty)");
    wprintf(L"  frame:            %lu x %lu, format=%lu (%ls), samples=%lu\n",
            static_cast<unsigned long>(ipc->width),
            static_cast<unsigned long>(ipc->height),
            static_cast<unsigned long>(format), FormatName(format),
            static_cast<unsigned long>(ipc->sampleCount));
    wprintf(L"  adapter LUID:     0x%016llX\n",
            static_cast<unsigned long long>(ipc->adapterLuid));
    wprintf(L"  sharing mode:     %lu (%ls)\n",
            static_cast<unsigned long>(ipc->sharingMode),
            SharingModeName(ipc->sharingMode));
    wprintf(L"  resource request: %lld\n",
            static_cast<long long>(ipc->resourceRequestId));
    wprintf(L"  host requested:   %lu (%ls)\n",
            static_cast<unsigned long>(ipc->hostRequestedSharingMode),
            SharingModeName(ipc->hostRequestedSharingMode));
    wprintf(L"  host response:    generation=%lld request=%lld controller=%lu mode=%lu (%ls)\n",
            static_cast<long long>(ipc->hostGeneration),
            static_cast<long long>(ipc->hostRequestId),
            static_cast<unsigned long>(ipc->hostControllerPid),
            static_cast<unsigned long>(ipc->hostResponseSharingMode),
            SharingModeName(ipc->hostResponseSharingMode));
    wprintf(L"  game NT attempt:  0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->producerNtError)),
            ResourceStageName(ipc->producerNtStage));
    wprintf(L"  game legacy try:  0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->producerLegacyError)),
            ResourceStageName(ipc->producerLegacyStage));
    wprintf(L"  game plain try:   0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->producerPlainError)),
            ResourceStageName(ipc->producerPlainStage));
    wprintf(L"  host ring result: 0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->hostResourceError)),
            ResourceStageName(ipc->hostFailureStage));
    wprintf(L"  host NT name:     0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->hostNtNamePathError)),
            ResourceStageName(ipc->hostNtNamePathStage));
    wprintf(L"  host NT handle:   0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->hostNtHandlePathError)),
            ResourceStageName(ipc->hostNtHandlePathStage));
    wprintf(L"  host legacy:      0x%08lX at %ls\n",
            static_cast<unsigned long>(static_cast<uint32_t>(ipc->hostLegacyPathError)),
            ResourceStageName(ipc->hostLegacyPathStage));
    wprintf(L"  target HWND:      0x%016llX\n",
            static_cast<unsigned long long>(ipc->targetHwnd));
    wprintf(L"  latest slot:      %ld\n", static_cast<long>(ipc->latestSlot));
    wprintf(L"  generation:       %lld\n", static_cast<long long>(ipc->generation));
    wprintf(L"  frame ID:         %lld\n", static_cast<long long>(ipc->frameId));
    wprintf(L"  slot sequences:   [%lld, %lld, %lld]\n",
            static_cast<long long>(ipc->slotSequence[0]),
            static_cast<long long>(ipc->slotSequence[1]),
            static_cast<long long>(ipc->slotSequence[2]));
    wprintf(L"  slot ACKs:        [%lld@%lld, %lld@%lld, %lld@%lld]\n",
            static_cast<long long>(ipc->slotAckSequence[0]),
            static_cast<long long>(ipc->slotAckRingId[0]),
            static_cast<long long>(ipc->slotAckSequence[1]),
            static_cast<long long>(ipc->slotAckRingId[1]),
            static_cast<long long>(ipc->slotAckSequence[2]),
            static_cast<long long>(ipc->slotAckRingId[2]));
    wprintf(L"  presents:         %lld\n", static_cast<long long>(ipc->presentCount));
    wprintf(L"  bootstrap calls:  %lld (target rejects=%lld)\n",
            static_cast<long long>(ipc->bootstrapPresentCount),
            static_cast<long long>(ipc->targetRejectedPresentCount));
    wprintf(L"  copies submitted: %lld\n", static_cast<long long>(ipc->copiesSubmitted));
    wprintf(L"  rate skipped:     %lld\n", static_cast<long long>(ipc->rateSkipped));
    wprintf(L"  busy dropped:     %lld\n", static_cast<long long>(ipc->busyDropped));
    wprintf(L"  rewrap count:     %lld\n", static_cast<long long>(ipc->rewrapCount));
    wprintf(L"  QPC last frame:   %lld\n", static_cast<long long>(ipc->qpcLastFrame));
    wprintf(L"  QPC frequency:    %lld\n", static_cast<long long>(ipc->qpcFrequency));
    wprintf(L"  max hook ticks:   %lld\n", static_cast<long long>(ipc->hookCpuTicksMax));
    wprintf(L"  stop requested:   %ld\n", static_cast<long>(ipc->stopRequested));

    const bool valid = ipc->magic == FCS_MAGIC &&
                       ipc->version == FCS_VERSION &&
                       ipc->bytes == sizeof(FcsIpcV1);
    if (!valid) {
        wprintf(L"  WARNING: mapping protocol does not match this source tree.\n");
    }

    UnmapViewOfFile(ipc);
    CloseHandle(mapping);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        char* end = nullptr;
        const unsigned long requested = strtoul(argv[1], &end, 10);
        if (!requested || !end || *end) {
            fwprintf(stderr, L"Usage: FcsStatusDump.exe [pid]\n");
            return 2;
        }
        return DumpForPid(static_cast<DWORD>(requested)) ? 0 : 3;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"Could not enumerate processes; error=%lu\n",
                 static_cast<unsigned long>(GetLastError()));
        return 2;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool foundGame = false;
    bool foundMapping = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (IsFfxivDx11(entry.szExeFile)) {
                foundGame = true;
                if (DumpForPid(entry.th32ProcessID)) {
                    foundMapping = true;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!foundGame) {
        wprintf(L"No running ffxiv_dx11.exe process was found.\n");
        return 1;
    }
    return foundMapping ? 0 : 3;
}
