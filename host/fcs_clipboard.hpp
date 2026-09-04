#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fcs::host {

bool CopyUnicodeTextToClipboard(HWND owner, const wchar_t* text);

} // namespace fcs::host
