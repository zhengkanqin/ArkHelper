#pragma once

#include <windows.h>

namespace ark::loader {

// True when the loader's own token is elevated.
bool is_elevated();

// Starts this executable again through the UAC "runas" verb. Returns true when the elevated
// instance was started; otherwise stores the Windows error in `error`.
bool relaunch_elevated(DWORD& error);

} // namespace ark::loader
