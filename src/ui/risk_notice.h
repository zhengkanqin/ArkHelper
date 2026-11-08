#pragma once
#include "../shell/platform.h"
#include "../config/config.h"

namespace arkhelp::risk_notice {
inline constexpr wchar_t kWindowClass[] = L"ArkHelper.RiskNotice";
// Blocks startup until this notice is explicitly acknowledged and saved.
// Closing or declining returns false; no game connection is enabled here.
bool ensure_accepted(HINSTANCE instance, const std::wstring& config_path, Config& config);
}
