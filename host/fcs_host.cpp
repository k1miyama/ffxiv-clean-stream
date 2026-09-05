#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fcs_controller.hpp"
#include "fcs_ui.hpp"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    using fcs::host::UiText;
    const auto language = fcs::host::LoadUiLanguage();
    // A capture session has one controller/consumer. Two controller processes
    // would otherwise race over the bidirectional resource handshake and the
    // keyed-mutex ring, so exclude them before either can claim an IPC page.
    // Keep the singleton name stable across protocol revisions so an older
    // controller cannot race a newer one against the same game process.
    HANDLE singleton =
        CreateMutexW(nullptr, FALSE, L"Local\\FCS3.Controller.Singleton");
    if (!singleton) {
        MessageBoxW(
            nullptr,
            UiText(language, L"FFXIV Clean Stream could not reserve its controller session."),
            L"FFXIV Clean Stream", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, UiText(language, L"FFXIV Clean Stream is already running."),
                    L"FFXIV Clean Stream", MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleton);
        return 0;
    }

    SetProcessDPIAware();
    fcs::host::HostController controller(instance);
    controller.SetLanguage(language);
    if (!fcs::host::CreateHostWindows(instance, showCommand, controller)) {
        CloseHandle(singleton);
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    const int exitCode = static_cast<int>(message.wParam);
    CloseHandle(singleton);
    return exitCode;
}
