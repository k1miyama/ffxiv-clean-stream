#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stddef.h>

namespace fcs::host {

struct TargetInfo {
    DWORD pid;
    HWND window;
};

bool FindFfxiv(TargetInfo& result);
bool InjectHook(DWORD pid, wchar_t* error, size_t errorChars);

} // namespace fcs::host
