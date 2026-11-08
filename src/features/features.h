// In-world overlays drawn on top of the game client area: spawn countdowns, the enemy card and
// floating damage numbers. Every function here is pure rendering: it reads the battle snapshot and
// writes pixels into the surface. Nothing blocks, waits or touches the shared memory directly.
#pragma once

#include <cstdint>
#include <string>

#include "arkhelp/ipc.h"
#include "../gfx/gfx.h"
#include "../config/config.h"

namespace arkhelp::features {

struct PointerState { float x=0, y=0; bool valid=false, clicked=false; };
inline bool pointer_near(const PointerState& pointer,float x,float y,float scale) {
    const float dx=pointer.x-x,dy=pointer.y-y;
    const float radius=48.0f*scale;
    return pointer.valid && dx*dx+dy*dy<=radius*radius;
}

// F1: planned spawn countdowns. Entries with a screen position get an integer second counter drawn
// in place; entries without one are listed on the right edge.
void draw_spawn_timers(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                       bool only_paused, bool paused, float font_size = 0);

// F2: a single card for the spawn selected by `card_index` (wraps around the spawn list).
void draw_spawn_card(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot,
                     std::uint32_t card_index, float scale);

// F3: floating damage numbers. Only the newest row per actor is shown; rows without a screen
// position are listed in the top left corner.
void draw_damage_numbers(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                         std::uint32_t metric_mask, bool merged, int sort_metric,
                         const int order[6]);
void draw_hp_bars(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                  bool only_paused, bool paused, float font_size = 0,
                  const OverlayOptions& options = {}, const PointerState& pointer = {});
void draw_operator_stats(gfx::Surface& surface,const ipc::BattleSnapshot& snapshot,float scale,
                         float font_size,const OverlayOptions& options,const PointerState& pointer);
void draw_enemy_stats(gfx::Surface& surface,const ipc::BattleSnapshot& snapshot,float scale,
                      float font_size,const OverlayOptions& options,const PointerState& pointer);

// Confirmed ranged enemies with a supported runtime shape, projected onto the map ground plane.
void draw_enemy_ranges(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                       const FeatureAppearance& appearance = {});

// "850", "1.2k", "12k", "120k", "1.2M" - integer math only, no printf float paths.
std::wstring format_damage(std::int64_t value);

// Color for an ipc::DamageType value.
gfx::Color damage_color(std::uint32_t damage_type);

}  // namespace arkhelp::features
