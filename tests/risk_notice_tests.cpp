#include "ui/risk_notice_state.h"
#include "config/config.h"
#include "shell/platform.h"
#include <cstdio>
#include <filesystem>
#include <stdexcept>

void check(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}
int main() {
    using namespace arkhelp::risk_notice;
    try {
        ReadingGate gate;
        check(!gate.can_accept(true), "checkbox cannot skip reading");
        gate.observe(100, true);
        gate.observe(10100, true);
        check(!gate.can_accept(true), "exactly ten seconds is insufficient");
        gate.observe(10101, true);
        check(!gate.can_accept(false), "reading cannot replace explicit checkbox");
        check(gate.can_accept(true), "over ten seconds plus checkbox should unlock");
        gate.observe(10200, false);
        check(gate.can_accept(true), "completed reading should remain valid");
        ReadingGate interrupted;
        interrupted.observe(100, true); interrupted.observe(9000, true);
        interrupted.observe(9050, false); interrupted.observe(9100, true);
        interrupted.observe(19100, true);
        check(!interrupted.can_accept(true), "focus loss must restart continuous reading");
        interrupted.observe(19101, true);
        check(interrupted.can_accept(true), "new continuous interval unlocks");
        const auto path = std::filesystem::temp_directory_path() /
            (L"arkhelper-risk-config-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        arkhelp::Config config;
        check(!arkhelp::config_load(path.wstring(), config) && config.risk_notice_version == 0,
            "fresh install must not be acknowledged");
        config.auto_launch = false; config.overlay.hp_offset_x = 75;
        check(arkhelp::config_save(path.wstring(), config), "save old/unacknowledged config");
        check(arkhelp::config_load(path.wstring(), config) && config.risk_notice_version == 0,
            "existing configs must not migrate to accepted");
        config.risk_notice_version = kNoticeVersion;
        check(arkhelp::config_save(path.wstring(), config), "save explicit acknowledgement");
        check(arkhelp::config_load(path.wstring(), config) && config.risk_notice_version == kNoticeVersion &&
            !config.auto_launch && config.overlay.hp_offset_x == 75, "persist notice and preserve preferences");
        std::filesystem::remove(path);
        std::puts("PASS risk notice: strict time gate, continuous focus, checkbox, fresh and existing config, persistence");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL risk notice: %s\n", error.what()); return 1;
    }
}
