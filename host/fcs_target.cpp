#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <wchar.h>

#include "fcs_target.hpp"

namespace fcs::host {
namespace {

struct WindowSearch {
    DWORD pid;
    HWND best;
    LONG64 area;
};

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM value) {
    WindowSearch* search = reinterpret_cast<WindowSearch*>(value);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER)) {
        return TRUE;
    }
    RECT client{};
    if (!GetClientRect(window, &client)) return TRUE;
    const LONG64 area = static_cast<LONG64>(client.right - client.left) *
                        (client.bottom - client.top);
    if (area > search->area) {
        search->area = area;
        search->best = window;
    }
    return TRUE;
}

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

bool BuildSiblingPath(wchar_t* output, size_t outputChars,
                      const wchar_t* fileName) {
    DWORD length = GetModuleFileNameW(
        nullptr, output, static_cast<DWORD>(outputChars));
    if (!length || length >= outputChars) return false;
    wchar_t* slash = wcsrchr(output, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    return wcscat_s(output, outputChars, fileName) == 0;
}

} // namespace

bool FindFfxiv(TargetInfo& result) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ffxiv_dx11.exe") == 0) {
                WindowSearch search{entry.th32ProcessID, nullptr, 0};
                EnumWindows(FindWindowCallback,
                            reinterpret_cast<LPARAM>(&search));
                if (search.best) {
                    result.pid = entry.th32ProcessID;
                    result.window = search.best;
                    found = true;
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool InjectHook(DWORD pid, wchar_t* error, size_t errorChars) {
    wchar_t dllPath[MAX_PATH]{};
    if (!BuildSiblingPath(dllPath, MAX_PATH,
                          L"FfxivCleanStreamHook64.dll") ||
        GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wcsncpy_s(error, errorChars,
                  L"FfxivCleanStreamHook64.dll is missing beside the app.",
                  _TRUNCATE);
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_DUP_HANDLE,
        FALSE, pid);
    if (!process) {
        wcsncpy_s(
            error, errorChars,
            L"Windows denied access to FFXIV. Run this app at the same privilege level as FFXIV.",
            _TRUNCATE);
        return false;
    }

    const size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath ||
        !WriteProcessMemory(process, remotePath, dllPath, bytes, nullptr)) {
        wcsncpy_s(error, errorChars,
                  L"The capture helper path could not be sent to FFXIV.",
                  _TRUNCATE);
        if (remotePath) VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = GetProcAddress(localKernel, "LoadLibraryW");
    const uintptr_t remoteKernel =
        FindRemoteModuleBase(pid, L"kernel32.dll");
    const uintptr_t loadOffset =
        reinterpret_cast<uintptr_t>(localLoadLibrary) -
        reinterpret_cast<uintptr_t>(localKernel);
    auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteKernel + loadOffset);
    HANDLE thread = remoteKernel
        ? CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary,
                             remotePath, 0, nullptr)
        : nullptr;
    if (!thread) {
        wcsncpy_s(error, errorChars,
                  L"Windows could not start the standalone capture helper.",
                  _TRUNCATE);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    // On timeout the remote thread may still be reading the path. A one-path
    // allocation leak is safer than freeing memory beneath LoadLibraryW.
    if (wait == WAIT_OBJECT_0) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    if (wait != WAIT_OBJECT_0 || exitCode == 0) {
        wcsncpy_s(error, errorChars,
                  L"FFXIV did not load the standalone capture helper.",
                  _TRUNCATE);
        return false;
    }
    return true;
}

} // namespace fcs::host
