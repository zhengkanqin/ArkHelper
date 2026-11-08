// Worker-owned current battle model. Managed objects stay in capture on the Unity thread;
// this class only consumes copied data, and retains exactly one current stage.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ark/plugin.h>

#include "../data/pack.h"

namespace ark::arkhelper {

class App {
public:
    explicit App(const ArkPluginApiV1& host);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    const ArkPluginApiV1& host() const { return host_; }
    bool test_host() const { return host_.test_host != 0; }

    // Resets the dynamic model. No external stage index/catalog is loaded.
    void load_data();
    void log(std::uint32_t level, const std::string& message) const;

    bool loaded() const { return loaded_; }
    const StagePack& pack() const { return pack_; }
    void refresh_runtime_stage(const std::string& level_id, std::uint32_t battle_serial);
    const Stage* selected_stage() const;

private:
    const ArkPluginApiV1& host_;
    StagePack pack_;
    bool loaded_ = false;
    std::uint32_t runtime_serial_ = 0;
    std::uint32_t runtime_capture_revision_ = 0;
};

} // namespace ark::arkhelper
