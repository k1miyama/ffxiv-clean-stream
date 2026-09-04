#include "fcs_internal.hpp"

extern "C" __declspec(dllexport) DWORD WINAPI FcsHookVersion(void*) {
    return FCS_VERSION;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        fcs::hook::InitializeRuntimeState(instance);
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(
            nullptr, 0, fcs::hook::HookWorker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

