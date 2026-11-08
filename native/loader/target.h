#pragma once

#include <windows.h>

namespace ark::loader {

// The game is identified by its fixed image name; no install path is configured.
inline constexpr wchar_t kGameImageName[] = L"Arknights.exe";

// Bundled D3D11 host used by --self-test in place of the game.
inline constexpr wchar_t kTestHostImageName[] = L"ArkHelperRuntimeHost.exe";

// A process selected for loading. A non-zero creation time pins the identity so a reused
// PID can never be mistaken for the selected process.
struct Target {
    DWORD pid = 0;
    ULONGLONG creation_time = 0;
    const wchar_t* image_name = kGameImageName;
};

// Checks the image name and, when known, the creation time of `process` against `target`.
// On mismatch sets the last error (ERROR_INVALID_NAME / ERROR_INVALID_PARAMETER).
bool verify_target_identity(HANDLE process, const Target& target);

// True when any process with the game's image name currently exists.
bool is_game_running();

} // namespace ark::loader
