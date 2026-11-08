#include "overlay_window.h"
#include "visibility.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>

#include "../features/features.h"
#include "../features/motion_predictor.h"
#include "../features/path_animation.h"
#include "../gfx/gfx.h"
#include "../ipc/battle_client.h"
#include "../log/log.h"
#include "arkhelp/ipc.h"

namespace arkhelp::overlay {
namespace {

constexpr wchar_t kWindowClass[] = L"ArkHelpOverlayWindow";
constexpr wchar_t kWindowTitle[] = L"ArkHelper Overlay";
constexpr UINT kRefreshMessage = WM_APP + 0x41;
constexpr UINT_PTR kTrackTimerId = 1;
constexpr UINT_PTR kRenderTimerId = 2;
constexpr UINT kTrackIntervalMs = 250;
constexpr UINT kRenderIntervalMs = 16;
constexpr ULONGLONG kMinFrameIntervalMs = 33;
constexpr int kMinClientExtent = 64;
constexpr int kDemoX = 300;
constexpr int kDemoY = 200;
constexpr int kDemoWidth = 800;
constexpr int kDemoHeight = 600;

struct FrameKey {
    std::uint64_t published_ms = 0;
    std::uint64_t content_stamp = 0;
    std::uint32_t battle_serial = 0;
    std::uint32_t spawn_count = 0;
    std::uint32_t enemy_count = 0;
    std::uint32_t damage_count = 0;
    std::uint32_t feature_mask = 0;
    std::uint32_t card_index = 0;
    std::uint32_t command_revision = 0;
    std::uint32_t pause_flags = 0;
    std::uint32_t widget_revision = 0;
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t scale_permille = 0;
};

bool operator==(const FrameKey& left, const FrameKey& right) {
    return left.published_ms == right.published_ms && left.content_stamp == right.content_stamp &&
           left.battle_serial == right.battle_serial && left.spawn_count == right.spawn_count &&
           left.enemy_count == right.enemy_count && left.damage_count == right.damage_count &&
           left.feature_mask == right.feature_mask && left.card_index == right.card_index &&
           left.command_revision == right.command_revision && left.pause_flags == right.pause_flags &&
           left.widget_revision == right.widget_revision && left.origin_x == right.origin_x &&
           left.origin_y == right.origin_y && left.width == right.width &&
           left.height == right.height && left.scale_permille == right.scale_permille;
}

gfx::Surface g_surface;
ipc::BattleClient g_battle;
ipc::CommandClient g_command;
HWND g_window = nullptr;
HINSTANCE g_instance = nullptr;
HWND g_game_window = nullptr;
HWINEVENTHOOK g_location_hook = nullptr;
HWINEVENTHOOK g_foreground_hook = nullptr;
HWINEVENTHOOK g_minimize_hook = nullptr;
HWINEVENTHOOK g_visibility_hook = nullptr;
LONG g_refresh_pending = 0;
bool g_demo = false;
bool g_visible = false;
bool g_frame_key_valid = false;
FrameKey g_frame_key = {};
ULONGLONG g_last_render_ms = 0;
ULONGLONG g_last_window_scan_ms = 0;
features::UnitMotionPredictor g_unit_motion;
features::PathAnimation g_paths;
features::PointerState g_pointer;
OverlayOptions g_options;
bool g_mouse_down=false;
std::function<void(std::uint32_t,std::span<const std::uint64_t>)> g_path_request_callback;

struct GameWindowSearch {
    HWND window = nullptr;
    LONGLONG area = 0;
};

void copy_field(char* destination, std::size_t capacity, const char* text) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    std::size_t length = 0;
    while (text[length] != '\0' && length + 1 < capacity) {
        ++length;
    }
    std::memcpy(destination, text, length);
    destination[length] = '\0';
}

bool equals_ignore_case(const wchar_t* left, const wchar_t* right) {
    while (*left != L'\0' && *right != L'\0') {
        const wint_t a = std::towlower(static_cast<wint_t>(*left));
        const wint_t b = std::towlower(static_cast<wint_t>(*right));
        if (a != b) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == L'\0' && *right == L'\0';
}

bool apply_dpi_awareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
        const auto raw =
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        const auto set_context = reinterpret_cast<SetDpiAwarenessContextFn>(raw);
        if (set_context != nullptr) {
            // (DPI_AWARENESS_CONTEXT)-4 is PER_MONITOR_AWARE_V2; the constant is not in every
            // MinGW header set, so the value is spelled out.
            if (set_context(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))) != FALSE) {
                return true;
            }
        }
    }
    return SetProcessDPIAware() != FALSE;
}

float dpi_scale_for_window() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto raw = reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow"));
        const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(raw);
        if (get_dpi != nullptr && g_window != nullptr) {
            const UINT dpi = get_dpi(g_window);
            if (dpi > 0) {
                return static_cast<float>(dpi) / 96.0f;
            }
        }
    }
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return 1.0f;
    }
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (dpi <= 0) {
        return 1.0f;
    }
    return static_cast<float>(dpi) / 96.0f;
}

void request_refresh() {
    if (g_window == nullptr) {
        return;
    }
    if (InterlockedCompareExchange(&g_refresh_pending, 1, 0) == 0) {
        PostMessageW(g_window, kRefreshMessage, 0, 0);
    }
}

void hide_overlay() {
    g_unit_motion.reset();
    g_paths.invalidate_projection(GetTickCount64());
    g_pointer={};
    g_mouse_down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    if(g_path_request_callback) g_path_request_callback(0,{});
    if (g_window != nullptr && g_visible) {
        ShowWindow(g_window, SW_HIDE);
    }
    g_visible = false;
    g_frame_key_valid = false;
}

bool game_is_foreground(HWND game) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || game == nullptr) {
        return false;
    }
    return GetAncestor(foreground, GA_ROOT) == GetAncestor(game, GA_ROOT);
}

BOOL CALLBACK enum_game_window(HWND window, LPARAM param) {
    auto* search = reinterpret_cast<GameWindowSearch*>(param);
    if (search == nullptr || !IsWindowVisible(window)) {
        return TRUE;
    }
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0) {
        return TRUE;
    }
    // 进程名是首选判据；但游戏以高完整性运行时，普通权限的 ArkHelper 无法
    // OpenProcess 查询镜像名，此时退回「Unity 窗口类 + 明日方舟标题」判据。
    bool matches = false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process != nullptr) {
        wchar_t path[1024] = {};
        DWORD path_length = static_cast<DWORD>(std::size(path));
        const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &path_length);
        CloseHandle(process);
        if (queried) {
            const wchar_t* name = std::wcsrchr(path, L'\\');
            name = name != nullptr ? name + 1 : path;
            matches = equals_ignore_case(name, L"Arknights.exe");
        }
    }
    if (!matches) {
        wchar_t class_name[64] = {};
        if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) > 0 &&
            equals_ignore_case(class_name, L"UnityWndClass")) {
            wchar_t title[128] = {};
            if (GetWindowTextW(window, title, static_cast<int>(std::size(title))) > 0 &&
                std::wcsstr(title, L"明日方舟") != nullptr) {
                matches = true;
            }
        }
    }
    if (!matches) {
        return TRUE;
    }
    RECT client = {};
    if (!GetClientRect(window, &client)) {
        return TRUE;
    }
    const LONGLONG area = static_cast<LONGLONG>(client.right - client.left) *
                          static_cast<LONGLONG>(client.bottom - client.top);
    // 最小化窗口客户区为 0，但仍应被识别为游戏窗口（是否绘制由可见性/前台判断决定）。
    if (search->window == nullptr || area > search->area) {
        search->area = area;
        search->window = window;
    }
    return TRUE;
}

HWND find_game_window() {
    GameWindowSearch search = {};
    // Fullscreen changes may create a new HWND while the old one is still alive.
    // Prefer the active game window over a hidden/retained swapchain host.
    enum_game_window(GetAncestor(GetForegroundWindow(), GA_ROOT), reinterpret_cast<LPARAM>(&search));
    if (search.window != nullptr) return search.window;
    EnumWindows(enum_game_window, reinterpret_cast<LPARAM>(&search));
    return search.window;
}


bool g_hp_only_paused = false;
bool g_spawn_only_paused = false;
bool g_range_only_paused = false;
std::uint32_t g_widget_revision = 0;
Config g_appearance_config;
std::uint32_t g_damage_metric_mask = 0x3Fu;
bool g_damage_merged = true;
int g_damage_sort_metric = 0;
int g_damage_order[6] = {0, 1, 2, 3, 4, 5};

void draw_scene(const ipc::BattleSnapshot& snapshot, const ipc::CommandState& commands,
                float scale) {
    const bool paused = (snapshot.flags & ipc::kBattlePaused) != 0;
    g_surface.begin_frame();
    const auto apply_style = [](std::uint32_t bit) -> const FeatureAppearance& {
        const auto& style = feature_appearance(g_appearance_config, bit);
        g_surface.set_style(style.opacity / 100.0f, style.text_color);
        return style;
    };
    if(commands.feature_mask&ipc::kFeatureEnemyPaths) {
        const auto& style=apply_style(ipc::kFeatureEnemyPaths);
        const auto rgb=style.text_color>=0?style.text_color:0x70dcff;
        features::draw_paths(g_surface,g_paths,scale,gfx::Color{
            ((rgb>>16)&255)/255.0f,((rgb>>8)&255)/255.0f,(rgb&255)/255.0f,1},style);
    }
    if ((commands.feature_mask & ipc::kFeatureEnemyRange) != 0 &&
        (!g_range_only_paused || paused)) {
        const auto& style=apply_style(ipc::kFeatureEnemyRange);
        features::draw_enemy_ranges(g_surface, snapshot, scale,style);
    }
    // R2: the projection path is live, so the in-world layers can be drawn again. Each feature is
    // gated by its own command bit, and every entry without a projected position falls back to the
    // list layout inside the feature itself.
    if ((commands.feature_mask & ipc::kFeatureSpawnTimer) != 0) {
        const auto& style = apply_style(ipc::kFeatureSpawnTimer);
        features::draw_spawn_timers(g_surface, snapshot, scale, g_spawn_only_paused, paused,
                                   static_cast<float>(style.font_size));
    }
    if ((commands.feature_mask & ipc::kFeatureSpawnCard) != 0) {
        apply_style(ipc::kFeatureSpawnList);
        features::draw_spawn_card(g_surface, snapshot, commands.card_index, scale);
    }
    // 数值统计只在悬浮窗面板里展示，游戏画面内不再绘制数字。
    if ((commands.feature_mask & ipc::kFeatureHpBars) != 0) {
        const auto& style = apply_style(ipc::kFeatureHpBars);
        features::draw_hp_bars(g_surface, snapshot, scale, g_hp_only_paused, paused,
                              static_cast<float>(style.font_size),g_options,g_pointer);
    }
    if(commands.feature_mask&ipc::kFeatureOperatorStats) {
        const auto& style=apply_style(ipc::kFeatureOperatorStats);
        features::draw_operator_stats(g_surface,snapshot,scale,static_cast<float>(style.font_size),g_options,g_pointer);
    }
    if(commands.feature_mask&ipc::kFeatureEnemyStats) {
        const auto& style=apply_style(ipc::kFeatureEnemyStats);
        features::draw_enemy_stats(g_surface,snapshot,scale,static_cast<float>(style.font_size),g_options,g_pointer);
    }
    g_surface.set_style(1.0f);
    g_surface.end_frame();
}

void present_scene(const ipc::BattleSnapshot& snapshot, const ipc::CommandState& commands, int x,
                   int y, int width, int height, float scale, std::uint64_t content_stamp) {
    if (g_window == nullptr) {
        return;
    }
    if (!g_demo && (!live_snapshot(snapshot, GetTickCount64()) ||
        !IsWindowVisible(g_game_window) || IsIconic(g_game_window) ||
        !game_is_foreground(g_game_window))) {
        hide_overlay();
        return;
    }
    if (!g_surface.resize(width, height)) {
        hide_overlay();
        return;
    }
    FrameKey key = {};
    key.published_ms = snapshot.published_ms;
    key.content_stamp = content_stamp;
    key.battle_serial = snapshot.battle_serial;
    key.spawn_count = snapshot.spawn_count;
    key.enemy_count = snapshot.enemy_count;
    key.damage_count = snapshot.damage_count;
    key.feature_mask = commands.feature_mask;
    key.card_index = commands.card_index;
    key.command_revision = commands.revision;
    key.pause_flags = snapshot.flags & ipc::kBattlePaused;
    key.widget_revision = g_widget_revision;
    key.origin_x = x;
    key.origin_y = y;
    key.width = width;
    key.height = height;
    key.scale_permille = static_cast<std::int32_t>(scale * 1000.0f + 0.5f);
    if (g_visible && g_frame_key_valid && key == g_frame_key) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    // A pause/resume or a widget edit must take effect on this UI tick, including when
    // positions and the game's logical frame have not changed yet.
    const bool control_changed = !g_frame_key_valid ||
        key.pause_flags != g_frame_key.pause_flags ||
        key.widget_revision != g_frame_key.widget_revision ||
        key.command_revision != g_frame_key.command_revision;
    if (g_visible && !control_changed && now - g_last_render_ms < kMinFrameIntervalMs) {
        return;
    }
    draw_scene(snapshot, commands, scale);
    // Focus can change while Direct2D draws. Never upload a completed frame over another app.
    if (!g_demo && (!IsWindowVisible(g_game_window) || IsIconic(g_game_window) ||
        !game_is_foreground(g_game_window))) {
        hide_overlay();
        return;
    }
    if (!g_visible) {
        ShowWindow(g_window, SW_SHOWNOACTIVATE);
        SetWindowPos(g_window, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    g_surface.present(g_window, x, y);
    g_visible = true;
    g_frame_key_valid = true;
    g_frame_key = key;
    g_last_render_ms = now;
}

void update_demo() {
    const ULONGLONG now = GetTickCount64();
    ipc::BattleSnapshot snapshot = {};
    snapshot.magic = ipc::kBattleMagic;
    snapshot.version = ipc::kVersion;
    snapshot.size = static_cast<std::uint32_t>(sizeof(ipc::BattleSnapshot));
    snapshot.sequence = 1;
    snapshot.published_ms = now;
    snapshot.battle_serial = 1;
    snapshot.in_battle = 1;
    snapshot.battle_state = 2;
    snapshot.flags = ipc::kBattleLevelKnown;
    snapshot.frame = static_cast<std::uint32_t>(now / 33);
    snapshot.time_seconds = 12.0;

    const float phase = static_cast<float>((now / 100) % 200) / 100.0f;
    snapshot.spawn_count = 5;
    snapshot.spawns[0].count = 2;
    snapshot.spawns[0].route_index = 1;
    snapshot.spawns[0].flags = ipc::kSpawnScreenValid;
    snapshot.spawns[0].screen.x = 250;
    snapshot.spawns[0].screen.y = 280;
    snapshot.spawns[0].remaining_seconds = 8.0f - phase;
    copy_field(snapshot.spawns[0].enemy_key, ipc::kIdCapacity, "enemy_1007_slime");
    copy_field(snapshot.spawns[0].enemy_name, ipc::kNameCapacity, "源石虫");

    snapshot.spawns[1].count = 1;
    snapshot.spawns[1].route_index = 2;
    snapshot.spawns[1].flags = ipc::kSpawnScreenValid;
    snapshot.spawns[1].screen.x = 360;
    snapshot.spawns[1].screen.y = 200;
    snapshot.spawns[1].remaining_seconds = 5.0f - phase;
    copy_field(snapshot.spawns[1].enemy_key, ipc::kIdCapacity, "enemy_1008_soldier");
    copy_field(snapshot.spawns[1].enemy_name, ipc::kNameCapacity, "士兵");

    snapshot.spawns[2].count = 1;
    snapshot.spawns[2].route_index = 3;
    snapshot.spawns[2].flags = ipc::kSpawnScreenValid;
    snapshot.spawns[2].screen.x = 260;
    snapshot.spawns[2].screen.y = 430;
    snapshot.spawns[2].remaining_seconds = 12.0f - phase;
    copy_field(snapshot.spawns[2].enemy_key, ipc::kIdCapacity, "enemy_1134_frostnova");
    copy_field(snapshot.spawns[2].enemy_name, ipc::kNameCapacity, "霜星");

    snapshot.spawns[3].count = 3;
    snapshot.spawns[3].route_index = 4;
    snapshot.spawns[3].flags = 0;
    snapshot.spawns[3].remaining_seconds = 9.0f - phase;
    copy_field(snapshot.spawns[3].enemy_key, ipc::kIdCapacity, "enemy_1012_drone");
    copy_field(snapshot.spawns[3].enemy_name, ipc::kNameCapacity, "无人机");

    snapshot.spawns[4].count = 5;
    snapshot.spawns[4].route_index = 5;
    snapshot.spawns[4].flags = ipc::kSpawnRandom;
    snapshot.spawns[4].remaining_seconds = -1.0f;
    copy_field(snapshot.spawns[4].enemy_key, ipc::kIdCapacity, "enemy_random_group");
    copy_field(snapshot.spawns[4].enemy_name, ipc::kNameCapacity, "敌群");

    snapshot.enemy_count = 2;
    snapshot.enemies[0].flags = ipc::kEnemyValid | ipc::kEnemyAlive;
    snapshot.enemies[0].hp = 320;
    snapshot.enemies[0].max_hp = 550;
    copy_field(snapshot.enemies[0].enemy_key, ipc::kIdCapacity, "enemy_1007_slime");
    copy_field(snapshot.enemies[0].enemy_name, ipc::kNameCapacity, "源石虫");
    snapshot.enemies[1].flags = ipc::kEnemyValid | ipc::kEnemyAlive;
    snapshot.enemies[1].hp = 18200;
    snapshot.enemies[1].max_hp = 25000;
    copy_field(snapshot.enemies[1].enemy_key, ipc::kIdCapacity, "enemy_1134_frostnova");
    copy_field(snapshot.enemies[1].enemy_name, ipc::kNameCapacity, "霜星");

    snapshot.damage_count = 6;
    struct DemoDamage {
        const char* id;
        const char* name;
        std::uint32_t type;
        std::int64_t total;
        std::int32_t x;
        std::int32_t y;
        bool on_screen;
    };
    const DemoDamage demo_damage[6] = {
        {"char_103_angel", "能天使", ipc::kDamagePhysical, 1200, 420, 480, true},
        {"char_291_aglina", "艾雅法拉", ipc::kDamageArts, 3400, 600, 500, true},
        {"char_350_surtr", "史尔特尔", ipc::kDamageTrue, 850, 520, 540, true},
        {"char_102_estelle", "华法琳", ipc::kDamageHeal, 12000, 360, 440, true},
        {"char_134_ifrit", "伊芙利特", ipc::kDamageElemental, 2000, 700, 470, true},
        {"char_172_silverash", "银灰", ipc::kDamagePhysical, 1200000, 0, 0, false},
    };
    for (int index = 0; index < 6; ++index) {
        ipc::DamageEntry& entry = snapshot.damage[index];
        entry.flags = ipc::kDamageValid;
        if (demo_damage[index].on_screen) {
            entry.flags |= ipc::kDamageScreenValid;
            entry.screen.x = demo_damage[index].x;
            entry.screen.y = demo_damage[index].y;
        }
        entry.damage_type = demo_damage[index].type;
        entry.total_damage = demo_damage[index].total;
        copy_field(entry.actor_id, ipc::kIdCapacity, demo_damage[index].id);
        copy_field(entry.actor_name, ipc::kNameCapacity, demo_damage[index].name);
    }

    ipc::CommandState commands = {};
    commands.magic = ipc::kCommandMagic;
    commands.version = ipc::kVersion;
    commands.size = static_cast<std::uint32_t>(sizeof(ipc::CommandState));
    commands.sequence = 1;
    commands.feature_mask = ipc::kFeatureSpawnTimer | ipc::kFeatureSpawnCard |
                            ipc::kFeatureDamageNumbers;
    commands.card_index = 2;
    commands.ui_scale = 1.0f;
    commands.revision = 1;

    const float scale = dpi_scale_for_window();
    present_scene(snapshot, commands, kDemoX, kDemoY, kDemoWidth, kDemoHeight, scale,
                  static_cast<std::uint64_t>(now / 33));
}

void update_live() {
    const ULONGLONG now = GetTickCount64();
    const bool stale_window = !IsWindow(g_game_window);
    if (stale_window || ((!IsWindowVisible(g_game_window) || IsIconic(g_game_window) ||
         !game_is_foreground(g_game_window)) && now - g_last_window_scan_ms >= kTrackIntervalMs)) {
        g_last_window_scan_ms = now;
        const HWND current = find_game_window();
        if (current != g_game_window) {
            hide_overlay();
            g_game_window = current;
            DWORD process_id = 0;
            if (current) GetWindowThreadProcessId(current, &process_id);
            log_info("overlay game window refreshed: hwnd=%p pid=%lu", current, process_id);
        }
    }
    if (g_game_window == nullptr) {
        hide_overlay();
        return;
    }
    if (!IsWindowVisible(g_game_window) || IsIconic(g_game_window) ||
        !game_is_foreground(g_game_window)) {
        hide_overlay();
        return;
    }
    RECT client = {};
    if (!GetClientRect(g_game_window, &client)) {
        hide_overlay();
        return;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width < kMinClientExtent || height < kMinClientExtent) {
        hide_overlay();
        return;
    }
    POINT origin = {};
    if (!ClientToScreen(g_game_window, &origin)) {
        hide_overlay();
        return;
    }
    ipc::BattleSnapshot snapshot = {};
    const bool have_snapshot = g_battle.poll(snapshot);
    ipc::CommandState commands = {};
    const bool have_commands = g_command.poll(commands);
    if (!have_commands) {
        commands = {};
        commands.magic = ipc::kCommandMagic;
        commands.version = ipc::kVersion;
        commands.size = static_cast<std::uint32_t>(sizeof(ipc::CommandState));
        commands.feature_mask = 0;
        commands.ui_scale = 1.0f;
    }
    const std::uint32_t draw_mask = ipc::kFeatureSpawnTimer | ipc::kFeatureSpawnCard |
                                    ipc::kFeatureHpBars | ipc::kFeatureEnemyRange |
                                    ipc::kFeatureOperatorStats | ipc::kFeatureEnemyPaths | ipc::kFeatureEnemyStats;
    const bool fresh = can_draw_live(snapshot, have_snapshot, IsWindowVisible(g_game_window),
                                     IsIconic(g_game_window), game_is_foreground(g_game_window),
                                     width, height, now);
    static const bool debug_live = [] {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(L"ARKHELP_OVERLAY_DEBUG", value,
                                                     static_cast<DWORD>(std::size(value)));
        return length > 0 && value[0] != L'0' && value[0] != L'\0';
    }();
    if (debug_live) {
        static ULONGLONG last_debug_ms = 0;
        if (now - last_debug_ms >= 1000) {
            last_debug_ms = now;
            const ipc::SpawnEntry* first = snapshot.spawn_count > 0 ? &snapshot.spawns[0] : nullptr;
            log_info("overlay live: have=%d in_battle=%d flags=0x%x age=%llu fresh=%d mask=0x%x "
                     "spawns=%u first=(flags=0x%x rem=%.1f screen=%d,%d) units=%u "
                     "unit0=(screen=%.0f,%.0f hp=%.0f/%.0f)",
                     have_snapshot ? 1 : 0, snapshot.in_battle, snapshot.flags,
                     static_cast<unsigned long long>(now - snapshot.published_ms), fresh ? 1 : 0,
                     commands.feature_mask, snapshot.spawn_count,
                     first ? first->flags : 0u, first ? first->remaining_seconds : 0.0f,
                     first ? first->screen.x : 0, first ? first->screen.y : 0,
                     snapshot.unit_count,
                     snapshot.unit_count > 0 ? snapshot.units[0].x : 0.0f,
                     snapshot.unit_count > 0 ? snapshot.units[0].y : 0.0f,
                     snapshot.unit_count > 0 ? snapshot.units[0].hp : 0.0f,
                     snapshot.unit_count > 0 ? snapshot.units[0].max_hp : 0.0f);
        }
    }
    if (!fresh || (commands.feature_mask & draw_mask) == 0) {
        hide_overlay();
        return;
    }
    float scale=dpi_scale_for_window()*std::clamp(commands.ui_scale>0?commands.ui_scale:1.0f,.5f,3.0f);
    if ((commands.feature_mask & (ipc::kFeatureHpBars | ipc::kFeatureEnemyRange | ipc::kFeatureEnemyPaths | ipc::kFeatureEnemyStats)) != 0) {
        g_unit_motion.apply(snapshot, now, width, height);
        if(g_unit_motion.projection_changed()) g_paths.invalidate_projection(now);
    } else {
        g_unit_motion.reset();
    }
    POINT cursor{};
    g_pointer={};
    if(GetCursorPos(&cursor)) {
        g_pointer.x=static_cast<float>(cursor.x-origin.x);
        g_pointer.y=static_cast<float>(cursor.y-origin.y);
        g_pointer.valid=g_pointer.x>=0 && g_pointer.y>=0 && g_pointer.x<width && g_pointer.y<height;
    }
    const bool mouse_down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    g_pointer.clicked=mouse_down && !g_mouse_down && g_pointer.valid;
    g_mouse_down=mouse_down;
    if(commands.feature_mask&ipc::kFeatureEnemyPaths) {
        g_paths.update(snapshot,g_pointer,scale,now,width,height,g_options);
        if(g_path_request_callback) g_path_request_callback(snapshot.battle_serial,g_paths.requests());
    } else {
        g_paths.reset();
        if(g_path_request_callback) g_path_request_callback(0,{});
    }
    // 性能：血量/伤害数字跟随会带来高频刷新。这里按「内容指纹」跳过无变化帧，
    // 同时限制到最多 30fps，避免每帧都重建并上传整窗位图。
    std::uint64_t fingerprint = 1469598103934665603ull;
    const auto mix = [&fingerprint](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            fingerprint ^= bytes[index];
            fingerprint *= 1099511628211ull;
        }
    };
    mix(&commands.feature_mask, sizeof(commands.feature_mask));
    mix(&commands.revision, sizeof(commands.revision));
    mix(&commands.ui_scale, sizeof(commands.ui_scale));
    mix(&snapshot.flags, sizeof(snapshot.flags));
    mix(&g_widget_revision, sizeof(g_widget_revision));
    mix(&snapshot.unit_count, sizeof(snapshot.unit_count));
    mix(snapshot.units, sizeof(ipc::UnitEntry) * std::min(snapshot.unit_count, ipc::kMaxUnits));
    if(commands.feature_mask&ipc::kFeatureEnemyStats) {
        mix(&snapshot.enemy_count,sizeof(snapshot.enemy_count));
        mix(snapshot.enemies,sizeof(ipc::EnemyEntry)*std::min(snapshot.enemy_count,ipc::kMaxEnemies));
    }
    mix(&snapshot.spawn_count, sizeof(snapshot.spawn_count));
    mix(snapshot.spawns, sizeof(ipc::SpawnEntry) * std::min(snapshot.spawn_count, ipc::kMaxSpawns));
    mix(&snapshot.damage_count, sizeof(snapshot.damage_count));
    mix(snapshot.damage,sizeof(ipc::DamageEntry)*std::min(snapshot.damage_count,ipc::kMaxDamageRows));
    if(g_options.hp_only_hover || g_options.operator_only_hover ||
       (commands.feature_mask & (ipc::kFeatureEnemyPaths | ipc::kFeatureEnemyStats))) {
        mix(&g_pointer.x,sizeof(g_pointer.x)); mix(&g_pointer.y,sizeof(g_pointer.y));
        mix(&g_pointer.valid,sizeof(g_pointer.valid));
    }
    if(commands.feature_mask&ipc::kFeatureEnemyPaths) {
        for(const auto& item:g_paths.playbacks()) {
            mix(&item.id,sizeof(item.id)); mix(&item.progress,sizeof(item.progress));
            mix(&item.ready,sizeof(item.ready));
        }
    }
    mix(&origin.x, sizeof(origin.x));
    mix(&origin.y, sizeof(origin.y));
    mix(&width, sizeof(width));
    mix(&height, sizeof(height));
    static std::uint64_t last_fingerprint = 0;
    static ULONGLONG last_present_ms = 0;
    if (g_visible && fingerprint == last_fingerprint && now - last_present_ms < 200) {
        return;
    }
    last_fingerprint = fingerprint;
    last_present_ms = now;

    // Prediction changes between IPC publications. Include the rendered positions in FrameKey so
    // its unchanged-publication fast path does not discard these intermediate frames.
    present_scene(snapshot, commands, origin.x, origin.y, width, height, scale, fingerprint);
}

void update_overlay() {
    if (g_demo) {
        update_demo();
    } else {
        update_live();
    }
}

void CALLBACK win_event_proc(HWINEVENTHOOK, DWORD event, HWND window, LONG object,
                              LONG, DWORD, DWORD) {
    if (g_demo) { return; }
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (!game_is_foreground(g_game_window)) { hide_overlay(); }
    } else if (window != g_game_window || object != OBJID_WINDOW) {
        return;
    } else if (event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_OBJECT_HIDE ||
               event == EVENT_OBJECT_DESTROY) {
        hide_overlay();
    }
    request_refresh();
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wparam == kTrackTimerId) {
                request_refresh();
                return 0;
            }
            if (wparam == kRenderTimerId) {
                update_overlay();
                return 0;
            }
            break;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            // Only rebuild this external drawing surface. A display transition never
            // submits a game launch/connection request or changes the game window.
            g_frame_key_valid = false;
            g_unit_motion.reset();
            g_paths.invalidate_projection(GetTickCount64());
            request_refresh();
            return 0;
        default:
            if (message == kRefreshMessage) {
                InterlockedExchange(&g_refresh_pending, 0);
                if (g_window == window) {
                    update_overlay();
                }
                return 0;
            }
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

bool initialize(HINSTANCE instance) {
    if (g_window != nullptr) {
        return true;
    }
    apply_dpi_awareness();
    wchar_t demo_value[8] = {};
    const DWORD demo_length = GetEnvironmentVariableW(L"ARKHELP_DEMO", demo_value,
                                                      static_cast<DWORD>(std::size(demo_value)));
    g_demo = demo_length > 0 && demo_length < std::size(demo_value) && demo_value[0] != L'0' &&
             demo_value[0] != L'\0';
    g_instance = instance;

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.style = 0;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass,
        kWindowTitle, WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
    if (g_window == nullptr) {
        return false;
    }
    if (!g_surface.initialize()) {
        DestroyWindow(g_window);
        g_window = nullptr;
        UnregisterClassW(kWindowClass, instance);
        return false;
    }
    g_last_render_ms = 0;
    g_frame_key_valid = false;
    if (SetTimer(g_window, kTrackTimerId, kTrackIntervalMs, nullptr) == 0) {
        shutdown();
        return false;
    }
    if (SetTimer(g_window, kRenderTimerId, kRenderIntervalMs, nullptr) == 0) {
        shutdown();
        return false;
    }
    g_location_hook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_foreground_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT);
    g_minimize_hook = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_visibility_hook = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    update_overlay();
    return true;
}

void shutdown() {
    if (g_window != nullptr) {
        KillTimer(g_window, kTrackTimerId);
        KillTimer(g_window, kRenderTimerId);
        ShowWindow(g_window, SW_HIDE);
        DestroyWindow(g_window);
        g_window = nullptr;
    }
    if (g_location_hook != nullptr) {
        UnhookWinEvent(g_location_hook);
        g_location_hook = nullptr;
    }
    if (g_foreground_hook != nullptr) {
        UnhookWinEvent(g_foreground_hook);
        g_foreground_hook = nullptr;
    }
    if (g_minimize_hook != nullptr) {
        UnhookWinEvent(g_minimize_hook);
        g_minimize_hook = nullptr;
    }
    if (g_visibility_hook != nullptr) {
        UnhookWinEvent(g_visibility_hook);
        g_visibility_hook = nullptr;
    }
    g_surface.shutdown();
    g_battle.close();
    g_command.close();
    if (g_instance != nullptr) {
        UnregisterClassW(kWindowClass, g_instance);
        g_instance = nullptr;
    }
    g_game_window = nullptr;
    g_visible = false;
    g_frame_key_valid = false;
    g_demo = false;
    g_refresh_pending = 0;
    g_last_render_ms = 0;
    g_unit_motion.reset();
}

bool active() {
    return g_window != nullptr;
}

void set_appearance(std::uint32_t feature, const FeatureAppearance& appearance) {
    auto& current = feature_appearance(g_appearance_config, feature);
    if (current == appearance) return;
    current = appearance;
    ++g_widget_revision;
    request_refresh();
}

void set_options(const OverlayOptions& options) {
    if(options.path_on_click!=g_options.path_on_click) g_paths.reset();
    g_options=options;
    ++g_widget_revision;
    request_refresh();
}

void set_path_request_callback(std::function<void(std::uint32_t,std::span<const std::uint64_t>)> callback) {
    g_path_request_callback=std::move(callback);
}

void set_widget_config(bool hp_only_paused, bool spawn_only_paused, bool range_only_paused) {
    if (g_hp_only_paused == hp_only_paused && g_spawn_only_paused == spawn_only_paused &&
        g_range_only_paused == range_only_paused) {
        return;
    }
    g_hp_only_paused = hp_only_paused;
    g_spawn_only_paused = spawn_only_paused;
    g_range_only_paused = range_only_paused;
    ++g_widget_revision;
    request_refresh();
}

void set_damage_config(std::uint32_t metric_mask, bool merged, int sort_metric,
                       const std::wstring& order) {
    g_damage_metric_mask = metric_mask;
    g_damage_merged = merged;
    g_damage_sort_metric = sort_metric;
    for (int i = 0; i < 6; ++i) {
        g_damage_order[i] = i;
    }
    int slot = 0;
    std::wstring current;
    auto flush = [&]() {
        if (slot >= 6 || current.empty()) {
            current.clear();
            return;
        }
        const int metric = current == L"total"      ? 0
                           : current == L"physical" ? 1
                           : current == L"arts"     ? 2
                           : current == L"true"     ? 3
                           : current == L"heal"     ? 4
                           : current == L"element"  ? 5
                                                    : -1;
        if (metric >= 0) {
            g_damage_order[slot++] = metric;
        }
        current.clear();
    };
    for (wchar_t ch : order) {
        if (ch == L',') {
            flush();
        } else if (ch != L' ' && ch != L'	') {
            current.push_back(ch);
        }
    }
    flush();
    while (slot < 6) {
        g_damage_order[slot] = slot;
        ++slot;
    }
}

}  // namespace arkhelp::overlay
