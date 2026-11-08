#include "app.h"
#include "../capture/capture.h"
#include <utility>

namespace ark::arkhelper {

App::App(const ArkPluginApiV1& host) : host_(host) {}

void App::load_data() {
    pack_ = StagePack{};
    loaded_ = true;
    runtime_serial_ = 0;
    runtime_capture_revision_ = 0;
    log(1, "ArkHelper: 动态关卡模式就绪（仅保存当前战斗，无需全量关卡包）");
}

void App::refresh_runtime_stage(const std::string& level_id, std::uint32_t battle_serial) {
    if (level_id.empty()) return;
    if (pack_.stages.empty() || runtime_serial_ != battle_serial ||
        pack_.stages.front().level_id != level_id) {
        Stage stage;
        stage.runtime = true;
        stage.loaded = false;
        stage.level_id = stage.stage_id = level_id;
        stage.name = "当前战斗";
        stage.revision = battle_serial;
        // Exactly one entry is retained. Same-level retries invalidate all prior values.
        pack_.stages.assign(1, std::move(stage));
        runtime_serial_ = battle_serial;
        runtime_capture_revision_ = 0;
    }
    Stage captured;
    std::uint32_t captured_serial = 0;
    if (!capture::runtime_stage(captured, &captured_serial, runtime_capture_revision_, runtime_serial_) || captured_serial != battle_serial ||
        captured.level_id != level_id) return;
    runtime_capture_revision_ = captured.revision;
    pack_.stages.front() = std::move(captured);
}

void App::log(std::uint32_t level, const std::string& message) const {
    if (host_.log && host_.owner) host_.log(host_.owner, level, message.c_str());
}
const Stage* App::selected_stage() const {
    return pack_.stages.empty() ? nullptr : &pack_.stages.front();
}
} // namespace ark::arkhelper
