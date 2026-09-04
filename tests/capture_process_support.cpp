#include "capture_e2e_support.hpp"

#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

namespace fcs_test {

bool BuildSiblingPath(wchar_t* output, size_t outputChars,
                      const wchar_t* fileName) {
    const DWORD length = GetModuleFileNameW(
        nullptr, output, static_cast<DWORD>(outputChars));
    if (!length || length >= outputChars) return false;
    wchar_t* slash = wcsrchr(output, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    return wcscat_s(output, outputChars, fileName) == 0;
}

namespace {

uintptr_t FindRemoteModuleBase(DWORD pid, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uintptr_t result = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

} // namespace

bool InjectHook(DWORD pid, const wchar_t* dllPath) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_DUP_HANDLE,
        FALSE, pid);
    if (!process) {
        wprintf(L"FAIL: OpenProcess for synthetic child (%lu)\n",
                GetLastError());
        return false;
    }

    const size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath ||
        !WriteProcessMemory(process, remotePath, dllPath, pathBytes, nullptr)) {
        wprintf(L"FAIL: copying hook path to synthetic child (%lu)\n",
                GetLastError());
        if (remotePath) VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = GetProcAddress(localKernel, "LoadLibraryW");
    const uintptr_t remoteKernel = FindRemoteModuleBase(pid, L"kernel32.dll");
    const uintptr_t loadOffset =
        reinterpret_cast<uintptr_t>(localLoadLibrary) -
        reinterpret_cast<uintptr_t>(localKernel);
    auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteKernel + loadOffset);
    HANDLE thread = remoteKernel
        ? CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary, remotePath,
                             0, nullptr)
        : nullptr;
    if (!thread) {
        wprintf(L"FAIL: starting LoadLibrary in synthetic child (%lu)\n",
                GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 10000);
    DWORD loadResult = 0;
    GetExitCodeThread(thread, &loadResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    if (waitResult != WAIT_OBJECT_0 || loadResult == 0) {
        wprintf(L"FAIL: hook DLL did not load in synthetic child\n");
        return false;
    }
    return true;
}

bool LaunchHiddenSyntheticGame(PROCESS_INFORMATION& processInfo) {
    wchar_t gamePath[MAX_PATH]{};
    if (!BuildSiblingPath(gamePath, MAX_PATH, L"SyntheticD3D11Game.exe")) {
        return false;
    }
    if (GetFileAttributesW(gamePath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"FAIL: SyntheticD3D11Game.exe is missing beside this test\n");
        return false;
    }
    wchar_t commandLine[MAX_PATH + 4]{};
    swprintf_s(commandLine, L"\"%s\"", gamePath);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    return !!CreateProcessW(gamePath, commandLine, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                            &processInfo);
}

} // namespace fcs_test
