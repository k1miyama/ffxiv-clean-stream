#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stddef.h>

namespace fcs::host {

enum class UiLanguage { English, SimplifiedChinese };

UiLanguage LanguageForWindowsUi(LANGID language);
UiLanguage LoadUiLanguage();
bool SaveUiLanguage(UiLanguage language);

// English source strings are the catalog keys. Unknown diagnostics are kept
// verbatim so a newer helper never loses information in an older controller.
const wchar_t* UiText(UiLanguage language, const wchar_t* english);
void LocalizeDiagnostic(UiLanguage language, const wchar_t* english,
                        wchar_t* output, size_t outputChars);

inline constexpr wchar_t kControlTitle[] =
    L"FFXIV Clean Stream — Standalone Controls";
inline constexpr wchar_t kIntroText[] =
    L"1. Start FFXIV and make sure the MMOMinion GUI is visible.\r\n"
    L"2. Choose your settings, then click Start / Resume.\r\n"
    L"3. In Discord, share ‘FFXIV Clean Stream’ — not FFXIV.";
inline constexpr wchar_t kInitialStatus[] =
    L"Nothing is attached. This app will not touch FFXIV until you click Start / Resume.";

} // namespace fcs::host
