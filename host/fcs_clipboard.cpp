#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <wchar.h>

#include "fcs_clipboard.hpp"

namespace fcs::host {

bool CopyUnicodeTextToClipboard(HWND owner, const wchar_t* text) {
    if (!text || !text[0]) return false;

    const size_t characterCount = wcslen(text) + 1;
    if (characterCount > SIZE_MAX / sizeof(wchar_t)) return false;
    const SIZE_T bytes = characterCount * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;

    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    CopyMemory(destination, text, bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    bool copied = false;
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
        // The clipboard owns the allocation after SetClipboardData succeeds.
        memory = nullptr;
        copied = true;
    }
    CloseClipboard();
    if (memory) GlobalFree(memory);
    return copied;
}

} // namespace fcs::host
