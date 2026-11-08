// Transparent click-through overlay that covers the game client area.
//
// The window is layered and never activates: the game keeps focus and receives every mouse and
// keyboard message while the overlay sits on top. Rendering happens on the thread that created the
// window (the UI thread that runs the message loop) and is throttled to roughly 30 frames per
// second. Nothing here blocks or touches the game process.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <string>
#include <functional>
#include <span>
#include <windows.h>
#include "../config/config.h"

namespace arkhelp::overlay {

// Creates the overlay window and starts the tracking/render timers. Must be called on the UI thread
// before the message loop starts. Returns false when the window or the Direct2D surface cannot be
// created; in that case the rest of ArkHelper keeps working and shutdown() is still safe to call.
bool initialize(HINSTANCE instance);

// Destroys the overlay, its timers, the WinEvent hooks and the Direct2D surface. Idempotent.
void shutdown();

// True between a successful initialize() and shutdown().
bool active();

// Feature widget options: draw only while the battle is paused.
void set_widget_config(bool hp_only_paused, bool spawn_only_paused, bool range_only_paused);
void set_appearance(std::uint32_t feature, const FeatureAppearance& appearance);
void set_options(const OverlayOptions& options);
void set_path_request_callback(std::function<void(std::uint32_t,std::span<const std::uint64_t>)> callback);

// 「数值统计」绘制选项：勾选指标、是否合并成一行、排序依据、指标顺序。
void set_damage_config(std::uint32_t metric_mask, bool merged, int sort_metric,
                       const std::wstring& order);

}  // namespace arkhelp::overlay
