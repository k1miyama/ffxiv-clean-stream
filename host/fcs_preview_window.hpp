#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fcs_stream_resolution.hpp"

namespace fcs::host {

class HostController;

HWND CreatePreviewWindow(HINSTANCE instance, HostController& controller,
                         StreamResolution resolution);
bool ResizePreviewWindowClient(HWND window, StreamResolution resolution);

} // namespace fcs::host
