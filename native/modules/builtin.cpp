#include "builtin.h"
#include "../core/plugin_manager.h"

extern "C" {
const ArkPluginInfoV1* arkhelp_damage_query();
bool arkhelp_damage_initialize(const ArkPluginApiV1*);
bool arkhelp_damage_shutdown();
bool arkhelp_damage_can_unload(char*, size_t);
const ArkPluginInfoV1* arkhelp_provider_query();
bool arkhelp_provider_initialize(const ArkPluginApiV1*);
bool arkhelp_provider_shutdown();
bool arkhelp_provider_can_unload(char*, size_t);
}

void arkhelp::runtime::register_product_modules(ark::core::PluginManager& manager) {
    manager.register_builtin({"damage_meter.dll", arkhelp_damage_query,
        arkhelp_damage_initialize, arkhelp_damage_shutdown, arkhelp_damage_can_unload});
    manager.register_builtin({"ark_helper.dll", arkhelp_provider_query,
        arkhelp_provider_initialize, arkhelp_provider_shutdown, arkhelp_provider_can_unload});
}
