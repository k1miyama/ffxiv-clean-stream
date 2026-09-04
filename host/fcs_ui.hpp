#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fcs::host {

class HostController;

bool CreateHostWindows(HINSTANCE instance, int showCommand,
                       HostController& controller);

} // namespace fcs::host
