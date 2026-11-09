#include "app.h"
#include "../data/battle_statistics.h"
#include "../data/panel_session.h"
#include "../data/enemy_overview.h"
#include "../data/wiki_preferences.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>

#include <objbase.h>
#include <tlhelp32.h>

#include <arkhelp/ipc.h>

#include "../config/config.h"
#include "../data/operator_names.h"
#include "../features/features.h"
#include "../game/game_launch.h"
#include "../log/log.h"
#include "../overlay/overlay_window.h"
#include "constants.h"
#include "ipc_client.h"
#include "status.h"
#include "theme.h"
#include "tray.h"
#include "win_util.h"
#include "../ui/ball.h"
#include "../ui/menu.h"
#include "../net/prts.h"
#include "../net/cache_directory.h"
#include "../ui/panels.h"
#include "../ui/risk_notice.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace arkhelp
{
namespace
{
template <std::size_t N>
void copy_char(char (&target)[N], const char* source)
{
    if (source == nullptr)
    {
        target[0] = '\0';
        return;
    }
    std::size_t index = 0;
    for (; index + 1 < N && source[index] != '\0'; ++index)
    {
        target[index] = source[index];
    }
    target[index] = '\0';
}
}  // namespace

namespace
{

std::wstring format_link_tooltip(LinkState state);

bool equals_ignore_case(const wchar_t* left, const wchar_t* right)
{
    while (*left != L'\0' && *right != L'\0')
    {
        if (std::towlower(*left) != std::towlower(*right))
        {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == L'\0' && *right == L'\0';
}

struct GameWindowSearch
{
    HWND window = nullptr;
    LONGLONG area = 0;
};

// 游戏以高完整性运行时，普通权限进程无法 OpenProcess 查询镜像名；
// 因此窗口枚举判据以「Unity 窗口类 + 明日方舟标题」兜底。
BOOL CALLBACK enum_game_window(HWND window, LPARAM param)
{
    auto* search = reinterpret_cast<GameWindowSearch*>(param);
    if (search == nullptr || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
    {
        return TRUE;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0)
    {
        return TRUE;
    }
    bool matches = false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process != nullptr)
    {
        wchar_t path[1024] = {};
        DWORD length = static_cast<DWORD>(std::size(path));
        const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        if (queried)
        {
            const wchar_t* name = std::wcsrchr(path, L'\\');
            name = name != nullptr ? name + 1 : path;
            matches = equals_ignore_case(name, L"Arknights.exe");
        }
    }
    if (!matches)
    {
        wchar_t class_name[64] = {};
        if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) > 0 &&
            equals_ignore_case(class_name, L"UnityWndClass"))
        {
            wchar_t title[128] = {};
            if (GetWindowTextW(window, title, static_cast<int>(std::size(title))) > 0 &&
                std::wcsstr(title, L"明日方舟") != nullptr)
            {
                matches = true;
            }
        }
    }
    if (!matches)
    {
        return TRUE;
    }
    RECT client = {};
    if (!GetClientRect(window, &client))
    {
        return TRUE;
    }
    const LONGLONG area = static_cast<LONGLONG>(client.right - client.left) *
                          static_cast<LONGLONG>(client.bottom - client.top);
    // 最小化时客户区为 0：仍然算「检测到游戏」，只是 overlay 不应绘制；
    // 首个匹配窗口直接接受，后续仅在面积更大时替换。
    if (search->window == nullptr || area > search->area)
    {
        search->area = area;
        search->window = window;
    }
    return TRUE;
}

// 进程名大小写不敏感的前缀比较：客户端可执行文件名在不同渠道略有差异
// （Arknights.exe / Arknights_CN.exe），匹配前缀即可覆盖。
bool equals_ignore_case_prefix(const wchar_t* text, const wchar_t* prefix)
{
    while (*prefix != L'\0')
    {
        if (*text == L'\0')
        {
            return false;
        }
        if (std::towlower(*text) != std::towlower(*prefix))
        {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

COLORREF damage_type_color(std::uint32_t damage_type)
{
    switch (damage_type)
    {
    case 1:
        return theme::kDanger; // 物理
    case 2:
        return theme::kAccent; // 法术
    case 3:
        return theme::kTextPrimary; // 真实
    case 4:
        return RGB(94, 210, 138); // 治疗
    case 5:
        return RGB(180, 132, 255); // 元素
    default:
        return theme::kTextPrimary;
    }
}

std::wstring narrow_to_wide(const char* text)
{
    return utf8_to_wide(text != nullptr ? text : "");
}

// App 是各窗口/组件之间的协调者：窗口不知道配置路径，配置层不知道窗口，
// 所有跨组件动作都收敛到这里的回调里，便于保证「改内存 → 改共享内存 → 落盘」的顺序。
class App
{
public:
    static App& instance();

    int initialize(HINSTANCE instance);
    int run();
    void shutdown();

private:
    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool create_shell_window();
    static LRESULT CALLBACK shell_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_shell_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void poll();
    void update_enemy_panels(const ipc::BattleSnapshot& snapshot, bool available, ULONGLONG now);
    void refresh_game_process();
    void refresh_link_state();
    void refresh_tray_tooltip();

    void show_menu();
    void show_settings();
    void toggle_ball();

    void on_feature_toggle(uint32_t bit, bool enabled);
    void on_only_paused(uint32_t bit, bool enabled);
    void on_opacity_changed(int percent, bool commit);
    void on_ui_scale_changed(int percent);
    void on_reset_defaults();
    void apply_appearances();
    std::filesystem::path cache_directory() const { return prts_cache_directory(root_path_,config_.prts_cache_path); }
    void configure_prts_cache() {
        prts_.configure_cache(cache_directory(),static_cast<std::uint64_t>(config_.prts_cache_mb)*1024*1024);
    }
    void on_exit_requested();
    void on_ball_drag_finished(int x, int y, int edge);

    // Submit launch/connection to the installed agent; preserve a running game.
    void request_launch();

    void save_config();
    void sync_commands_from_config();

    HINSTANCE instance_ = nullptr;
    HWND shell_hwnd_ = nullptr;
    HANDLE instance_mutex_ = nullptr;
    HANDLE ready_mutex_ = nullptr;
    bool class_registered_ = false;
    bool com_initialized_ = false;
    bool quitting_ = false;
    bool tray_active_ = false;

    Config config_;
    std::wstring root_path_;
    std::wstring config_dir_;
    std::wstring config_path_;
    std::wstring log_dir_;

    Status status_;
    CommandClient commands_;
    BattleReader battle_;
    StageOverviewReader overview_reader_;
    ipc::StageOverview overview_;
    EnemyOverview enemy_overview_;
    PanelSession panel_session_;
    PanelWidthRevision damage_width_revision_;
    struct EnemyView {
        int selected = 0, enemy_rows = 0, action_row = -1;
        std::string selected_key;
        std::vector<std::string> keys;
        std::vector<std::wstring> names;
        PanelAutoWidth auto_width;
    };
    std::array<EnemyView, 2> enemy_views_;
    ULONGLONG wiki_request_ms_ = 0;
    OperatorNames operator_names_;
    net::PrtsClient prts_;
    std::wstring wiki_name_;
    net::PrtsResult wiki_result_;
    std::uint32_t wiki_battle_serial_ = 0;
    std::map<std::wstring, HBITMAP> wiki_avatar_cache_;      // 敌人名 -> 48px 头像位图
    std::map<std::wstring, bool> wiki_avatar_requested_;     // 已发起的头像请求
    std::map<std::wstring, ULONGLONG> wiki_avatar_retry_;
    std::map<std::wstring, unsigned> wiki_avatar_failures_;

    ULONGLONG last_battle_attempt_ = 0;
    ULONGLONG last_process_refresh_ = 0;
    bool auto_launch_attempted_ = false;
    HANDLE runtime_stop_event_ = nullptr;
    bool game_seen_ = false;   // 本次会话是否见过游戏：见过就绝不自动重启
    bool watch_armed_once_ = false;
};

App& App::instance()
{
    static App app;
    return app;
}

bool App::create_shell_window()
{
    if (!class_registered_)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &App::shell_proc;
        window_class.hInstance = instance_;
        window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
        window_class.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        window_class.lpszClassName = constants::kShellClass;
        if (RegisterClassExW(&window_class) == 0)
        {
            log_error("RegisterClassExW(shell) failed, code=%d", static_cast<int>(GetLastError()));
            return false;
        }
        class_registered_ = true;
    }

    // 隐藏的协调窗口：不显示，仅供托盘回调、定时器和第二实例唤醒使用。
    shell_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, constants::kShellClass, constants::kAppTitle, WS_POPUP, 0, 0, 0,
                                  0, nullptr, nullptr, instance_, nullptr);
    if (shell_hwnd_ == nullptr)
    {
        log_error("CreateWindowExW(shell) failed, code=%d", static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

int App::initialize(HINSTANCE instance)
{
    instance_ = instance;

    // 单实例：多开会导致托盘图标重复、两个进程抢同一份命令区，因此第二个实例只负责唤起旧实例。
    // CreateMutexW 在“新建成功”时不会清空 last error，所以先手动清零，避免误判已有实例。
    SetLastError(0);
    instance_mutex_ = CreateMutexW(nullptr, FALSE, constants::kMutexName);
    if (instance_mutex_ == nullptr)
    {
        MessageBoxW(nullptr, L"创建单实例互斥体失败，ArkHelper 无法启动。", constants::kAppName, MB_OK | MB_ICONERROR);
        return 2;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 旧实例可能刚拿到互斥体、窗口还没建好，短暂重试后再放弃，避免“点了没反应”。
        HWND existing = nullptr;
        for (int attempt = 0; attempt < 10 && existing == nullptr; ++attempt)
        {
            existing = FindWindowW(constants::kShellClass, constants::kAppTitle);
            if (existing == nullptr)
            {
                Sleep(100);
            }
        }
        if (existing != nullptr)
        {
            PostMessageW(existing, constants::kMsgWake, 0, 0);
        }
        else if (HWND notice = FindWindowW(risk_notice::kWindowClass, nullptr))
        {
            ShowWindow(notice, SW_SHOW);
            SetForegroundWindow(notice);
        }
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
        // 返回 1 表示“已有实例在跑，本进程正常退出”；不能返回 0，否则外层会进入消息循环挂住。
        return 1;
    }

    // 配置和日志与 EXE 放在同一便携目录。
    root_path_ = app_root();
    config_dir_ = join_path(root_path_, L"config");
    log_dir_ = join_path(root_path_, L"logs");
    ensure_directory(config_dir_);
    ensure_directory(log_dir_);
    log_open(log_dir_);
    log_info("ArkHelper starting, root=%s", wide_to_utf8(root_path_).c_str());
    // An in-place UI update can reconnect without opening a game the user just closed.
    // This launch-only override never changes the user's saved auto-launch preference.
    if (const char* connect_only = std::getenv("ARKHELP_CONNECT_ONLY");
        connect_only && *connect_only && *connect_only != '0') {
        auto_launch_attempted_ = true;
        log_info("connect-only desktop session: automatic game creation skipped");
    }

    // COM 目前没有强依赖，但托盘/文件对话框等系统组件以后可能需要；失败只降级不阻塞。
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(com_result);
    if (!com_initialized_)
    {
        log_warn("CoInitializeEx failed, hr=0x%08lX", static_cast<unsigned long>(com_result));
    }

    config_path_ = join_path(config_dir_, L"arkhelp.json");
    const bool config_loaded = config_load(config_path_, config_);
    log_info("config damage: merged=%d mask=%lu sort=%d order=%s loaded=%d",
             config_.damage_merged ? 1 : 0, (unsigned long)config_.damage_metric_mask,
             config_.damage_sort_metric, wide_to_utf8(config_.damage_order).c_str(),
             config_loaded ? 1 : 0);
    if (!config_loaded)
    {
        config_ = config_defaults();
    }
    // No ready marker, game watcher, IPC, overlay or network work exists until
    // the user explicitly accepts. Legacy configurations require the notice too.
    if (!risk_notice::ensure_accepted(instance_, config_path_, config_)) {
        log_info("first-use risk notice declined or closed; game connection remains disabled");
        return 10;
    }
    ready_mutex_ = CreateMutexW(nullptr, FALSE, constants::kReadyMutexName);
    if (!ready_mutex_) return 2;
    (void)overlay::initialize(instance_);
    overlay::set_widget_config(config_.hp_only_paused, config_.spawn_only_paused, config_.range_only_paused);
    overlay::set_damage_config(config_.damage_metric_mask, config_.damage_merged,
                               config_.damage_sort_metric, config_.damage_order);
    log_info("config %s, ball=(%d,%d) edge=%d features=0x%08X opacity=%d ui_scale=%d",
             config_loaded ? "loaded" : "defaults", config_.ball_x, config_.ball_y, config_.edge,
             static_cast<unsigned>(config_.features), config_.opacity, config_.ui_scale);

    // 解析游戏/加载器/启动器路径；解析成功后写回配置，用户下次启动无需再猜路径。
    game_launch::initialize(root_path_, config_);
    // 退出 ArkHelper 时通过命名事件让游戏内运行时自行关闭；启动时先复位，避免上次残留导致误停。
    runtime_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ArkHelp.RuntimeStop");
    if (runtime_stop_event_ != nullptr)
    {
        ResetEvent(runtime_stop_event_);
    }
    const game_launch::Paths& resolved = game_launch::paths();
    const bool paths_changed = config_.game_path != resolved.game_exe || config_.loader_path != resolved.loader_exe;
    config_.game_path = resolved.game_exe;
    config_.loader_path = resolved.loader_exe;
    if (paths_changed && config_loaded) { save_config(); }
    if (!game_launch::paths_ready())
    {
        log_warn("game launch paths unresolved: %s", wide_to_utf8(game_launch::last_error()).c_str());
    }

    // 命令区必须由本进程创建（provider 只读打开），失败说明权限或名字冲突，先记错误继续跑。
    if (commands_.open())
    {
        log_info("command mapping ready");
    }
    else
    {
        log_error("command mapping open failed");
    }
    if (battle_.open())
    {
        log_info("battle mapping opened");
    }
    else
    {
        log_info("battle mapping not available yet, will retry while polling");
    }
    sync_commands_from_config();
    configure_prts_cache();
    overlay::set_options(config_.overlay);
    overlay::set_path_request_callback([this](std::uint32_t serial,std::span<const std::uint64_t> ids) {
        commands_.set_path_requests(serial,ids);
    });

    if (!create_shell_window())
    {
        return 3;
    }

    BallCallbacks ball_callbacks;
    ball_callbacks.on_left_click = [this]() { show_menu(); };
    ball_callbacks.on_right_click = [this]() { show_settings(); };
    ball_callbacks.on_drag_finished = [this](int x, int y, int edge) { on_ball_drag_finished(x, y, edge); };
    if (!ball_create(instance_, ball_callbacks, config_))
    {
        log_error("ball_create failed");
        return 4;
    }

    MenuCallbacks menu_callbacks;
    menu_callbacks.get_config = [this]() { return config_; };
    menu_callbacks.on_opacity = [this](int percent, bool commit) { on_opacity_changed(percent, commit); };
    menu_callbacks.on_ui_scale = [this](int percent) { on_ui_scale_changed(percent); };
    menu_callbacks.on_reset = [this]() { on_reset_defaults(); };
    menu_callbacks.on_menu_opacity = [this](int percent) {
        config_.menu_opacity = std::clamp(percent, 20, 100);
        save_config();
        menu_sync_config(config_);
    };
    menu_callbacks.on_menu_height = [this](int height) {
        config_.menu_height=std::clamp(height,400,900);save_config();
    };
    menu_callbacks.on_appearance = [this](uint32_t bit, const FeatureAppearance& appearance) {
        const bool font_changed = feature_appearance(config_, bit).font_size != appearance.font_size;
        feature_appearance(config_, bit) = appearance;
        apply_appearances();
        if (font_changed && bit == ipc::kFeatureDamageNumbers) damage_width_revision_ = {};
        if (font_changed && bit == ipc::kFeatureWiki) enemy_views_[0].auto_width.request();
        if (font_changed && bit == ipc::kFeatureEnemyPrts) enemy_views_[1].auto_width.request();
        save_config();
    };
    menu_callbacks.on_wiki_fields = [this](uint32_t bit, uint32_t mask) {
        if (bit == ipc::kFeatureEnemyPrts) config_.prts_fields = mask & kWikiAllFields;
        else config_.wiki_fields = mask & kWikiAllFields;
        enemy_views_[bit == ipc::kFeatureEnemyPrts ? 1 : 0].auto_width.request();
        save_config();
    };
    menu_callbacks.on_feature_menu = [this](uint32_t mask, const FeatureOrder& order) {
        config_.menu_features = mask & kMenuFeatureMask;
        config_.feature_order = order;
        config_.features &= config_.menu_features;
        status_.feature_mask = config_.features;
        sync_commands_from_config();
        save_config();
        menu_sync_config(config_);
        poll();
    };
    menu_callbacks.on_overlay_options = [this](const OverlayOptions& options) {
        config_.overlay=options;
        overlay::set_options(options);
        save_config();
    };
    menu_callbacks.on_prts_cache_limit = [this](int mb) {
        config_.prts_cache_mb=std::clamp(mb,10,5120);
        configure_prts_cache();
        save_config();
    };
    menu_callbacks.get_prts_cache_path = [this] { return cache_directory().wstring(); };
    menu_callbacks.on_prts_cache_path = [this](const std::wstring& path) -> std::wstring {
        const auto directory=prts_cache_directory(root_path_,path);
        const auto error=prepare_cache_directory(directory);
        if(!error.empty()) return error;
        config_.prts_cache_path=path.empty()?L"":directory.wstring();
        configure_prts_cache();
        save_config();
        return {};
    };
    menu_callbacks.on_prts_cache_open = [this]() -> std::wstring {
        const auto directory=cache_directory();
        const auto error=prepare_cache_directory(directory);
        if(!error.empty()) return error;
        const auto opened=ShellExecuteW(menu_is_visible()?GetForegroundWindow():shell_hwnd_,L"open",
            directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(opened)>32?L"":L"无法打开缓存目录";
    };
    menu_callbacks.on_auto_launch = [this](bool enabled) {
        config_.auto_launch = enabled;
        save_config();
    };
    menu_callbacks.get_status = [this]() -> const Status& { return status_; };
    menu_callbacks.get_feature_mask = [this]() { return status_.feature_mask; };
    menu_callbacks.on_feature_toggle = [this](uint32_t bit, bool enabled) { on_feature_toggle(bit, enabled); };
    menu_callbacks.get_only_paused = [this](uint32_t bit) {
        if (bit == ipc::kFeatureSpawnTimer) return config_.spawn_only_paused;
        if (bit == ipc::kFeatureHpBars) return config_.hp_only_paused;
        return bit == ipc::kFeatureEnemyRange && config_.range_only_paused;
    };
    menu_callbacks.on_only_paused = [this](uint32_t bit, bool enabled) {
        if (bit == ipc::kFeatureSpawnTimer) {
            config_.spawn_only_paused = enabled;
        } else if (bit == ipc::kFeatureHpBars) {
            config_.hp_only_paused = enabled;
        } else if (bit == ipc::kFeatureEnemyRange) {
            config_.range_only_paused = enabled;
        }
        overlay::set_widget_config(config_.hp_only_paused, config_.spawn_only_paused, config_.range_only_paused);
        save_config();
    };
    menu_callbacks.get_damage_config = [this]() {
        DamageConfig config;
        config.metric_mask = config_.damage_metric_mask;
        config.merged = config_.damage_merged;
        config.compact = config_.damage_compact;
        config.show_devices = config_.damage_show_devices;
        config.sort_metric = config_.damage_sort_metric;
        config.order = config_.damage_order;
        return config;
    };
    menu_callbacks.on_damage_config = [this](const DamageConfig& config) {
        if (config_.damage_metric_mask != config.metric_mask || config_.damage_merged != config.merged ||
            config_.damage_order != config.order || config_.damage_compact != config.compact) damage_width_revision_ = {};
        config_.damage_metric_mask = config.metric_mask;
        config_.damage_merged = config.merged;
        config_.damage_compact = config.compact;
        config_.damage_show_devices = config.show_devices;
        panels::set_statistics_compact(config.compact);
        config_.damage_sort_metric = config.sort_metric;
        config_.damage_order = config.order;
    overlay::set_damage_config(config_.damage_metric_mask, config_.damage_merged,
                                   config_.damage_sort_metric, config_.damage_order);
        save_config();
    };
    menu_callbacks.get_launch_busy = []() { return game_launch::busy(); };
    menu_callbacks.on_launch = [this]() { request_launch(); };
    menu_callbacks.on_open_settings = [this]() { show_settings(); };
    menu_callbacks.on_hidden = [this]() {
        if (!quitting_)
        {
            ball_show(true);
        }
    };
    menu_callbacks.on_exit = [this]() { on_exit_requested(); };
    if (!menu_create(instance_, menu_callbacks))
    {
        log_error("menu_create failed");
        return 5;
    }

    // 可拖拽信息面板：位置/折叠状态与配置双向同步；面板自身只负责绘制与拖动。
    operator_names_.load(module_dir());
    panels::Callbacks panel_callbacks;
    panel_callbacks.on_closed = [this](const wchar_t* id) {
        const auto bit = std::wcscmp(id, panels::kDamagePanelId) == 0 ? ipc::kFeatureDamageNumbers :
            std::wcscmp(id, panels::kSpawnPanelId) == 0 ? ipc::kFeatureSpawnList :
            std::wcscmp(id, panels::kPrtsPanelId) == 0 ? ipc::kFeatureEnemyPrts : ipc::kFeatureWiki;
        panel_session_.dismiss(bit);
    };
    panel_callbacks.on_state_changed = [this](const wchar_t* id, int x, int y, bool collapsed) {
        if (std::wcscmp(id, panels::kDamagePanelId) == 0)
        {
            config_.damage_panel_x = x;
            config_.damage_panel_y = y;
            config_.damage_panel_collapsed = collapsed;
        }
        else if (std::wcscmp(id, panels::kSpawnPanelId) == 0)
        {
            config_.spawn_panel_x = x;
            config_.spawn_panel_y = y;
            config_.spawn_panel_collapsed = collapsed;
        }
        else if (std::wcscmp(id, panels::kWikiPanelId) == 0)
        {
            config_.wiki_panel_x = x;
            config_.wiki_panel_y = y;
            config_.wiki_panel_collapsed = collapsed;
        }
        else if (std::wcscmp(id, panels::kPrtsPanelId) == 0) {
            config_.prts_panel_x = x; config_.prts_panel_y = y; config_.prts_panel_collapsed = collapsed;
        }
        save_config();
        log_info("panel %s moved to (%d,%d) collapsed=%d", wide_to_utf8(id).c_str(), x, y,
                 static_cast<int>(collapsed));
    };
    panel_callbacks.on_settings = [this](const wchar_t* id) {
        const auto bit=std::wcscmp(id,panels::kDamagePanelId)==0?ipc::kFeatureDamageNumbers:
            std::wcscmp(id,panels::kSpawnPanelId)==0?ipc::kFeatureSpawnList:
            std::wcscmp(id,panels::kPrtsPanelId)==0?ipc::kFeatureEnemyPrts:ipc::kFeatureWiki;
        if(!menu_is_visible()) show_menu();
        menu_open_config(bit);
    };
    panel_callbacks.on_row_clicked = [this](const wchar_t* id, int row_index) {
        if (std::wcscmp(id, panels::kWikiPanelId) != 0 && std::wcscmp(id, panels::kPrtsPanelId) != 0)
        {
            return;
        }
        const bool reference = std::wcscmp(id, panels::kPrtsPanelId) == 0;
        auto& view = enemy_views_[reference ? 1 : 0];
        const auto fields = reference ? config_.prts_fields : config_.wiki_fields;
        if (row_index >= 0 && row_index < view.enemy_rows)
        {
            if ((fields & kWikiAvatars) &&
                static_cast<std::size_t>(row_index) < view.names.size()) {
                const auto& name = view.names[static_cast<std::size_t>(row_index)];
                if (!wiki_avatar_cache_.count(name) && !wiki_avatar_requested_.count(name) &&
                    prts_.request_image(name, true, true)) {
                    wiki_avatar_requested_[name] = true;
                    wiki_avatar_retry_.erase(name);
                }
            }
            if (view.selected != row_index)
            {
                view.selected = row_index;
                view.selected_key = static_cast<std::size_t>(row_index) < view.keys.size()
                    ? view.keys[static_cast<std::size_t>(row_index)] : "";
                panels::reset_scroll(reference ? panels::kPrtsPanelId : panels::kWikiPanelId);
                if (reference) { wiki_result_ = {}; wiki_name_.clear(); }
            }
            return;
        }
        if (reference && row_index == view.action_row)
        {
            if (!wiki_name_.empty()) {
                wiki_result_ = {};
                wiki_request_ms_ = GetTickCount64();
                prts_.request(wiki_name_, true);
            }
        }
    };
    panel_callbacks.on_resized = [this](const wchar_t* id, int width_px, int height_px) {
        if (std::wcscmp(id, panels::kDamagePanelId) == 0)
        {
            config_.damage_panel_width = width_px;
            config_.damage_panel_height = height_px;
        }
        else if (std::wcscmp(id, panels::kSpawnPanelId) == 0)
        {
            config_.spawn_panel_width = width_px;
            config_.spawn_panel_height = height_px;
        }
        else if (std::wcscmp(id, panels::kWikiPanelId) == 0)
        {
            config_.wiki_panel_width = width_px;
            config_.wiki_panel_height = height_px;
            enemy_views_[0].auto_width.restore(width_px);
        }
        else if (std::wcscmp(id, panels::kPrtsPanelId) == 0) {
            config_.prts_panel_width = width_px; config_.prts_panel_height = height_px;
            enemy_views_[1].auto_width.restore(width_px);
        }
        save_config();
        log_info("panel %s resized to %dx%d px", wide_to_utf8(id).c_str(), width_px, height_px);
    };
    if (!panels::initialize(instance_, panel_callbacks))
    {
        log_error("panels initialize failed");
        return 7;
    }
    panels::restore_geometry(panels::kDamagePanelId, config_.damage_panel_x, config_.damage_panel_y,
                             config_.damage_panel_collapsed);
    panels::restore_geometry(panels::kSpawnPanelId, config_.spawn_panel_x, config_.spawn_panel_y,
                             config_.spawn_panel_collapsed);
    panels::restore_geometry(panels::kPrtsPanelId, config_.prts_panel_x, config_.prts_panel_y,
                             config_.prts_panel_collapsed);
    if (config_.wiki_panel_x != kConfigNoPosition && config_.wiki_panel_y != kConfigNoPosition)
    {
        panels::restore_geometry(panels::kWikiPanelId, config_.wiki_panel_x, config_.wiki_panel_y,
                                 config_.wiki_panel_collapsed);
    }
    else
    {
        panels::restore_geometry(panels::kWikiPanelId, -100000, -100000, true);
    }
    if (std::getenv("ARKHELP_PANEL_DEMO") != nullptr)
    {
        // 离线验收：把面板放到可见位置并展开，便于截图检查表格排版/磁吸联动（不影响真实使用）。
        // 演示环境无视 config 里被关掉的开关，保证三块面板都可见。
        config_.features |= static_cast<std::uint32_t>(ipc::kFeatureSpawnTimer) |
                            static_cast<std::uint32_t>(ipc::kFeatureDamageNumbers) |
                            static_cast<std::uint32_t>(ipc::kFeatureWiki);
    }
    panels::restore_height(panels::kDamagePanelId, config_.damage_panel_height);
    panels::restore_height(panels::kSpawnPanelId, config_.spawn_panel_height);
    panels::restore_height(panels::kWikiPanelId, config_.wiki_panel_height);
    panels::restore_height(panels::kPrtsPanelId, config_.prts_panel_height);
    panels::restore_width(panels::kDamagePanelId, config_.damage_panel_width);
    panels::restore_width(panels::kSpawnPanelId, config_.spawn_panel_width);
    panels::restore_width(panels::kWikiPanelId, config_.wiki_panel_width);
    panels::restore_width(panels::kPrtsPanelId, config_.prts_panel_width);
    enemy_views_[0].auto_width.restore(config_.wiki_panel_width);
    enemy_views_[1].auto_width.restore(config_.prts_panel_width);
    panels::restore_dock_state(config_.dock_state);
    if (std::getenv("ARKHELP_PANEL_DEMO") != nullptr)
    {
        // 离线验收：固定三块面板的位置/尺寸，便于截图检查排版与磁吸联动。
        panels::restore_geometry(panels::kDamagePanelId, 100, 100, false);
        panels::restore_height(panels::kDamagePanelId, 430);
        panels::restore_width(panels::kDamagePanelId, 320);
        panels::restore_geometry(panels::kSpawnPanelId, 100, 420, false);
        panels::restore_height(panels::kSpawnPanelId, 220);
        panels::restore_width(panels::kSpawnPanelId, 300);
        panels::restore_height(panels::kWikiPanelId, 840);
        panels::restore_width(panels::kWikiPanelId, 600);
        panels::restore_geometry(panels::kWikiPanelId, 560, 120, false);
    }
    panels::refresh_theme(config_.ui_scale);
    apply_appearances();

    TrayCallbacks tray_callbacks;
    tray_callbacks.toggle_ball = [this]() { toggle_ball(); };
    tray_callbacks.open_menu = [this]() { show_menu(); };
    tray_callbacks.open_settings = [this]() { show_settings(); };
    tray_callbacks.launch_game = [this]() { request_launch(); };
    tray_callbacks.exit_app = [this]() { on_exit_requested(); };
    const std::wstring tooltip = format_link_tooltip(status_.link);
    tray_active_ = tray_create(shell_hwnd_, tooltip.c_str(), tray_callbacks);
    if (!tray_active_)
    {
        log_warn("tray_create failed, running without tray icon");
    }

    SetTimer(shell_hwnd_, constants::kPollTimerId, constants::kPollIntervalMs, nullptr);
    poll();
    refresh_tray_tooltip();

    if (!config_loaded)
    {
        // 首次运行立刻落一份默认配置，方便用户/验收直接看到文件结构。
        save_config();
    }

    log_info("initialization complete, command_mapping=%s battle_mapping=%s",
             commands_.ready() ? "yes" : "no", battle_.is_open() ? "yes" : "no");
    return 0;
}

int App::run()
{
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    log_info("message loop finished");
    shutdown();
    return static_cast<int>(message.wParam);
}

void App::shutdown()
{
    log_info("shutting down");
    if (ready_mutex_) { CloseHandle(ready_mutex_); ready_mutex_ = nullptr; }
    // 请求游戏运行时停止。开发时可用 ARKHELP_KEEP_RUNTIME=1 保留会话。
    static const bool keep_runtime = [] {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(L"ARKHELP_KEEP_RUNTIME", value,
                                                     static_cast<DWORD>(std::size(value)));
        return length > 0 && value[0] != L'0' && value[0] != L'\0';
    }();
    if (runtime_stop_event_ != nullptr && !keep_runtime)
    {
        SetEvent(runtime_stop_event_);
    }
    save_config();
    if (shell_hwnd_ != nullptr)
    {
        KillTimer(shell_hwnd_, constants::kPollTimerId);
    }
    if (tray_active_)
    {
        tray_destroy();
        tray_active_ = false;
    }
    menu_destroy();
    prts_.shutdown();
    panels::shutdown();
    for (auto& [name, bitmap] : wiki_avatar_cache_)
    {
        (void)name;
        destroy_bitmap(bitmap);
    }
    wiki_avatar_cache_.clear();
    ball_destroy();
    if (shell_hwnd_ != nullptr)
    {
        DestroyWindow(shell_hwnd_);
        shell_hwnd_ = nullptr;
    }
    if (runtime_stop_event_ != nullptr)
    {
        CloseHandle(runtime_stop_event_);
        runtime_stop_event_ = nullptr;
    }
    commands_.close();
    battle_.close();
    overview_reader_.close();
    if (com_initialized_)
    {
        CoUninitialize();
        com_initialized_ = false;
    }
    log_info("shutdown complete");
    log_close();
    if (instance_mutex_ != nullptr)
    {
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
    }
}

void App::poll()
{
    const ULONGLONG now = GetTickCount64();

    // 启动器是独立提权进程，这里只轮询它的退出状态，失败时给出错误横幅。
    game_launch::poll();
    status_.launch_error = game_launch::last_error();

    // provider 可能比外壳晚启动，所以战斗区每秒重试一次打开，而不是只在启动时试一次。
    if (!battle_.is_open() && now - last_battle_attempt_ >= 1000)
    {
        last_battle_attempt_ = now;
        if (battle_.open())
        {
            log_info("battle mapping opened (retry)");
        }
    }

    if (now - last_process_refresh_ >= 2000)
    {
        last_process_refresh_ = now;
        refresh_game_process();
        // Development reviews leave real game processes untouched.
        const char* demo = std::getenv("ARKHELP_PANEL_DEMO");
        const char* no_launch = std::getenv("ARKHELP_NO_GAME_LAUNCH");
        const bool demo_mode = (demo && *demo) || (no_launch && *no_launch && *no_launch != '0');
        if (!demo_mode && !watch_armed_once_)
        {
            watch_armed_once_ = true;
            game_launch::arm_silent_watch();
        }
        if (!status_.game_process.empty())
        {
            game_seen_ = true;
            auto_launch_attempted_ = true;
        }
        else if (!config_.auto_launch)
        {
            auto_launch_attempted_ = true;
        }
        else if (!demo_mode && config_.auto_launch && !auto_launch_attempted_ && !game_seen_ &&
                 !game_launch::busy() && game_launch::paths_ready())
        {
            // The agent rechecks for a concurrent manual start before launching anything.
            // A failed request stays failed until an explicit retry, never a game restart loop.
            auto_launch_attempted_ = true;
            if (!game_launch::request_launch())
                log_error("automatic launch request failed: %s", wide_to_utf8(game_launch::last_error()).c_str());
        }
    }

    ipc::BattleSnapshot snapshot{};
    const bool battle_open = battle_.is_open();
    status_.battle_mapping_open = battle_open;
    status_.battle_valid = battle_open && battle_.read(snapshot);
    if (status_.battle_valid)
    {
        status_.in_battle = snapshot.in_battle != 0;
        status_.battle_state = snapshot.battle_state;
        status_.time_seconds = snapshot.time_seconds;
        status_.sequence = snapshot.sequence;
        status_.frame = snapshot.frame;
        // provider 写的是 char[] 而不是 std::string，先按 NUL 截断再转宽字符，避免越界。
        size_t length = 0;
        while (length < sizeof(snapshot.level_id) && snapshot.level_id[length] != '\0')
        {
            ++length;
        }
        status_.level_id.assign(snapshot.level_id, length);
    }
    else
    {
        status_.in_battle = false;
        status_.battle_state = -1;
        status_.time_seconds = 0.0;
        status_.level_id.clear();
    }

    // 开发验收：ARKHELP_PANEL_DEMO=1 时伪造一场战斗，便于离线检查 WIKI 面板（真实环境不会进入此分支）。
    if (const char* demo = std::getenv("ARKHELP_PANEL_DEMO"); demo != nullptr && *demo != '\0')
    {
        status_.battle_valid = true;
        status_.in_battle = true;
        status_.battle_state = 2;
        snapshot = ipc::BattleSnapshot{};
        snapshot.magic = ipc::kBattleMagic;
        snapshot.version = ipc::kVersion;
        snapshot.size = sizeof(ipc::BattleSnapshot);
        snapshot.in_battle = 1;
        snapshot.battle_state = 2;
        snapshot.frame = 1234;
        snapshot.battle_serial = 1;
        snapshot.published_ms = GetTickCount64();
        snapshot.time_seconds = 31.5;
        struct DemoEnemy
        {
            const char* key;
            const char* name;
            long long max_hp;
            long long atk;
            long long def;
        };
        const DemoEnemy demo_enemies[] = {
            {"enemy_1002_nsabr", "士兵", 1650, 200, 30},
            {"enemy_1007_slime", "源石虫", 550, 130, 0},
            {"enemy_1009_firebomb", "暴徒", 1700, 250, 50},
        };
        snapshot.enemy_count = 3;
        for (int index = 0; index < 3; ++index)
        {
            ipc::EnemyEntry& enemy = snapshot.enemies[index];
            copy_char(enemy.enemy_key, demo_enemies[index].key);
            copy_char(enemy.enemy_name, demo_enemies[index].name);
            enemy.max_hp = demo_enemies[index].max_hp;
            enemy.hp = demo_enemies[index].max_hp - 100;
            enemy.flags = ipc::kEnemyValid | ipc::kEnemyAlive | ipc::kEnemyRuntime;
            enemy.stats_mask = ipc::kEnemyHp | ipc::kEnemyMaxHp | ipc::kEnemyAtk | ipc::kEnemyDef;
            enemy.tracking_id = static_cast<std::uint64_t>(index + 1);
            enemy.atk = demo_enemies[index].atk;
            enemy.def = demo_enemies[index].def;
            enemy.damage_type_mask = 1u << 1;
        }
        // Exercise the production statistics renderer with a full self-contained snapshot.
        // This includes support-only actors and enough rows/columns to verify both scroll axes.
        const char* demo_actor_names[] = {"阿米娅","银灰","史尔特尔","能天使","伊芙利特","澄闪",
            "维什戴尔","玛恩纳","锏","逻各斯","铃兰","塞雷娅","夜莺","白面鸮","桃金娘","极境","风笛","投锋召唤物"};
        snapshot.damage_count = static_cast<std::uint32_t>(std::size(demo_actor_names));
        snapshot.team_metric_mask = statistics::kAllMetrics & ~(1u << ipc::kStatShield);
        double team_damage = 0;
        for (std::uint32_t index=0; index<snapshot.damage_count; ++index) {
            auto& entry=snapshot.damage[index];
            entry.flags=ipc::kDamageValid;
            entry.metric_mask=snapshot.team_metric_mask;
            const std::string id="demo_actor_"+std::to_string(index);
            copy_char(entry.actor_id,id.c_str());
            copy_char(entry.actor_name,demo_actor_names[index]);
            entry.metrics[ipc::kStatPhysical]=index<12 ? (18-index)*1350.0 : 0;
            entry.metrics[ipc::kStatArts]=index<12 && index%3==0 ? (16-index)*1650.0 : 0;
            entry.metrics[ipc::kStatTrue]=index<12 && index%4==0 ? 2800.0+index*230.0 : 0;
            entry.metrics[ipc::kStatElemental]=index<12 && index%5==0 ? 1800.0+index*130.0 : 0;
            entry.metrics[ipc::kStatDamage]=entry.metrics[ipc::kStatPhysical]+entry.metrics[ipc::kStatArts]+
                entry.metrics[ipc::kStatTrue]+entry.metrics[ipc::kStatElemental];
            entry.metrics[ipc::kStatHeal]=index>=12 && index<15 ? 18200.0-(index-12)*3000.0 : 0;
            entry.metrics[ipc::kStatCost]=index>=14 ? 42.0+(index-14)*7 : 0;
            entry.metrics[ipc::kStatBuildup]=index%4==0 ? 680.0+index*200.0 : 0;
            entry.metrics[ipc::kStatTaken]=index%3==0 ? 1200.0+index*270.0 : 0;
            entry.metrics[ipc::kStatDps]=entry.metrics[ipc::kStatDamage]/snapshot.time_seconds;
            entry.total_damage=static_cast<std::int64_t>(entry.metrics[ipc::kStatDamage]);
            team_damage+=entry.metrics[ipc::kStatDamage];
        }
        for (std::uint32_t index=0; index<snapshot.damage_count; ++index) {
            auto& entry=snapshot.damage[index];
            entry.metrics[ipc::kStatShare]=entry.metrics[ipc::kStatDamage]/team_damage;
            for (std::uint32_t metric=0;metric<ipc::kStatCount;++metric)
                snapshot.team_metrics[metric]+=entry.metrics[metric];
        }

    }

    status_.command_mapping = commands_.ready();
    if (commands_.ready())
    {
        status_.feature_mask = commands_.feature_mask();
        status_.revision = commands_.revision();
    }
    else
    {
        status_.feature_mask = config_.features;
    }

    refresh_link_state();

    // ---- 可拖拽信息面板：战斗中即可用，切出游戏（失焦）时保持显示，不随焦点消失。 ----
    panel_session_.observe(status_.battle_valid ? &snapshot : nullptr, GetTickCount64());
    const bool panel_data_available = panel_session_.phase() != PanelSession::Phase::kIdle;
    if (panel_data_available) {
        snapshot = panel_session_.snapshot();
        overview_reader_.read(overview_);
        enemy_overview_.observe(snapshot, overview_, panel_session_.generation());
    }
    for (const auto* id : {panels::kDamagePanelId, panels::kSpawnPanelId, panels::kWikiPanelId, panels::kPrtsPanelId})
        panels::set_closeable(id, panel_session_.retained());
    std::vector<panels::Row> damage_rows;
    std::vector<panels::Row> spawn_rows;
    if (panel_data_available)
    {
        const auto order = statistics::parse_order(config_.damage_order);
        std::vector<int> selected;
        for (int metric : order)
            if (config_.damage_metric_mask & (1u << metric)) selected.push_back(metric);
        if (selected.empty()) selected.push_back(ipc::kStatDamage);
        // Compact view keeps the selected headline metric; quantities with different units are never added.
        if (config_.damage_merged && selected.size() > 1) selected.resize(1);
        std::vector<const ipc::DamageEntry*> actors;
        const auto count = std::min(snapshot.damage_count, ipc::kMaxDamageRows);
        for (std::uint32_t index = 0; index < count; ++index)
            if (statistics::visible(snapshot.damage[index], config_.damage_show_devices)) actors.push_back(&snapshot.damage[index]);
        const int sort_metric = config_.damage_sort_metric > 0 &&
            config_.damage_sort_metric <= static_cast<int>(ipc::kStatCount)
            ? config_.damage_sort_metric - 1 : ipc::kStatDamage;
        std::stable_sort(actors.begin(), actors.end(), [sort_metric](const auto* a, const auto* b) {
            const double av = statistics::value(*a, sort_metric), bv = statistics::value(*b, sort_metric);
            return av == bv ? std::strcmp(a->actor_id, b->actor_id) < 0 : av > bv;
        });
        const auto damage_width_key = std::hash<std::wstring>{}(config_.damage_order) ^
            (static_cast<std::uint64_t>(config_.damage_metric_mask) << 32) ^
            (config_.damage_merged ? 1ull << 56 : 0) ^ (config_.damage_compact ? 1ull << 57 : 0);
        panels::Row header;
        header.header = true;
        header.cells.push_back(L"干员");
        for (int metric : selected) {
            std::wstring label = statistics::kMetrics[metric].label;
            if (metric == sort_metric) label += L" ↓";
            header.cells.push_back(std::move(label));
        }
        header.tip = L"总伤害仅合计物理、法术、真实、元素扣血；治疗、回费、元素积累分别统计。";
        damage_rows.push_back(std::move(header));
        const auto totals = statistics::team_metrics(snapshot, config_.damage_show_devices);
        const auto supported = snapshot.team_metric_mask;
        panels::Row team;
        team.header = true;
        team.accent = static_cast<unsigned>(theme::kAccent);
        team.cells.push_back(L"全队合计");
        for (int metric : selected)
            team.cells.push_back((supported & (1u << metric)) ? statistics::format(totals[metric], metric) : L"—");
        team.tip = L"全队合计包含所有已采集干员和召唤物；DPS按同一战斗时长计算。— 表示采集入口未接入。";
        damage_rows.push_back(std::move(team));
        if (snapshot.stats_flags) {
            panels::Row note;
            note.cells = {(snapshot.stats_flags & 1u) ? L"中途接入或事件丢失：当前为已采集部分" :
                                                       L"干员超过64：仅显示前64，全队合计仍完整"};
            damage_rows.push_back(std::move(note));
        }
        for (const auto* actor : actors) {
            panels::Row row;
            std::wstring name;
            if (const auto* mapped = operator_names_.find(actor->actor_id)) name = *mapped;
            else name = narrow_to_wide(actor->actor_name[0] ? actor->actor_name : actor->actor_id);
            row.cells.push_back(name);
            row.tip = name + L"（" + narrow_to_wide(actor->actor_id) + L"）";
            for (int metric : selected) {
                const bool available = (actor->metric_mask & (1u << metric)) != 0;
                const auto value = statistics::value(*actor, metric, totals[ipc::kStatDamage]);
                row.cells.push_back(available ? statistics::format(value, metric) : L"—");
                row.tip += L"\n" + std::wstring(statistics::kMetrics[metric].label) + L"：" +
                    (available ? statistics::format(value, metric, true) : L"采集入口未接入");
            }
            damage_rows.push_back(std::move(row));
        }
        if (actors.empty()) {
            panels::Row row;
            row.cells = {L"等待战斗事件…"};
            row.tip = L"尚未观察到干员伤害、治疗或回费事件。";
            damage_rows.push_back(std::move(row));
        }

        if ((status_.feature_mask & ipc::kFeatureDamageNumbers) && damage_width_revision_.changed(damage_width_key))
            panels::fit_statistics_width(damage_rows);

        const bool matching_overview = overview_.header.battle_serial == snapshot.battle_serial &&
            std::strncmp(overview_.header.level_id, snapshot.level_id, ipc::kLevelIdCapacity) == 0;
        if (matching_overview) {
            if (overview_.header.flags & ipc::kOverviewPartial) {
                panels::Row note;
                note.wrap = true; note.left = L"部分关卡分支暂未读取完整，以下为已读取的整关计划。";
                spawn_rows.push_back(std::move(note));
            }
            for (const auto& entry : overview_.spawns) {
                panels::Row row;
                row.left = (entry.branch > 0 ? L"分支 " + std::to_wstring(entry.branch) + L" · " : L"") +
                    L"波次 " + std::to_wstring(entry.wave + 1) + L" / " + std::to_wstring(entry.fragment + 1);
                if (entry.flags & ipc::kStageSpawnConditional) row.left += L" · 条件";
                const auto* enemy = enemy_overview_.find(entry.enemy_key);
                row.right = narrow_to_wide(enemy && enemy->enemy_name[0] ? enemy->enemy_name : entry.enemy_key);
                row.right += entry.flags & ipc::kStageSpawnCountKnown ? L" ×" + std::to_wstring(entry.count) : L" ×?";
                spawn_rows.push_back(std::move(row));
            }
        }
        if (spawn_rows.empty()) {
            panels::Row note;
            note.wrap = true;
            note.left = matching_overview && (overview_.header.flags & ipc::kOverviewPlanRead)
                ? L"本关未读取到出怪动作" : L"等待当前关卡的完整出怪计划…";
            spawn_rows.push_back(std::move(note));
        }
    }
    // 没有游戏时也能验收面板外观：ARKHELP_PANEL_DEMO=1 用假数据强制显示。
    bool panel_visible = panel_data_available;
    if (const char* demo = std::getenv("ARKHELP_PANEL_DEMO"); demo != nullptr && *demo != '\0')
    {
        panel_visible = true;
        if (spawn_rows.empty())
        {
            spawn_rows = {
                panels::Row{L"00:05", L"源石虫 ×2", 0},
                panels::Row{L"00:12", L"士兵", 0},
                panels::Row{L"00:21", L"暴徒·α", 0},
            };
        }
    }
    panels::update(panels::kDamagePanelId, L"数值统计", damage_rows,
                   panel_visible && panel_session_.visible(ipc::kFeatureDamageNumbers) &&
                   (status_.feature_mask & ipc::kFeatureDamageNumbers) != 0);
    const std::wstring spawn_title = L"出怪列表";
    panels::update(panels::kSpawnPanelId, spawn_title.c_str(), spawn_rows,
                   panel_visible && panel_session_.visible(ipc::kFeatureSpawnList) &&
                   (status_.feature_mask & ipc::kFeatureSpawnList) != 0);

    // 头像下载结果：后台线程只搬字节，WIC 解码放在 UI 线程（WIC 工厂不是线程安全的）。
    {
        std::wstring image_name;
        std::wstring image_error;
        std::vector<unsigned char> image_bytes;
        while (prts_.poll_image(image_name, image_bytes, &image_error))
        {
            wiki_avatar_requested_.erase(image_name);
            HBITMAP bitmap = image_bytes.empty() ? nullptr :
                load_image_memory(image_bytes.data(), image_bytes.size(), 48, 48);
            if (bitmap != nullptr)
            {
                wiki_avatar_failures_.erase(image_name);
                wiki_avatar_retry_.erase(image_name);
                const auto cached = wiki_avatar_cache_.find(image_name);
                if (cached != wiki_avatar_cache_.end())
                {
                    destroy_bitmap(cached->second);
                    cached->second = bitmap;
                }
                else
                {
                    wiki_avatar_cache_[image_name] = bitmap;
                }
            } else {
                if(!image_bytes.empty()) prts_.invalidate_image(image_name);
                constexpr ULONGLONG delays[] = {2000, 5000, 15000, 30000, 60000};
                auto& failures = wiki_avatar_failures_[image_name];
                const auto delay = delays[std::min(failures, 4u)];
                failures = std::min(failures + 1, 5u);
                wiki_avatar_retry_[image_name] = now + delay;
                log_warn("enemy avatar unavailable: %s; %s; retry in %llu ms",
                    wide_to_utf8(image_name).c_str(),
                    image_bytes.empty() ? wide_to_utf8(image_error).c_str() : "image decode failed",
                    static_cast<unsigned long long>(delay));
            }
        }
    }

    update_enemy_panels(snapshot, panel_data_available, now);

    if (menu_is_visible())
    {
        menu_refresh();
    }
}

void App::update_enemy_panels(const ipc::BattleSnapshot& snapshot, bool available, ULONGLONG now)
{
    if (available && wiki_battle_serial_ != snapshot.battle_serial) {
        wiki_battle_serial_ = snapshot.battle_serial;
        for (auto& view : enemy_views_) { view.selected = 0; view.selected_key.clear(); }
        panels::reset_scroll(panels::kWikiPanelId, true);
        panels::reset_scroll(panels::kPrtsPanelId, true);
        panels::set_avatars(panels::kWikiPanelId, {}, 28);
        panels::set_avatars(panels::kPrtsPanelId, {}, 28);
        for (auto& cached : wiki_avatar_cache_) destroy_bitmap(cached.second);
        wiki_avatar_cache_.clear(); wiki_avatar_requested_.clear();
        wiki_avatar_retry_.clear(); wiki_avatar_failures_.clear();
    }
    for (int view_index = 0; view_index < 2; ++view_index) {
        const bool reference = view_index == 1;
        auto& view = enemy_views_[view_index];
        const auto bit = reference ? ipc::kFeatureEnemyPrts : ipc::kFeatureWiki;
        const auto* panel_id = reference ? panels::kPrtsPanelId : panels::kWikiPanelId;
        const auto field_mask = (status_.feature_mask & bit) ? (reference ? config_.prts_fields : config_.wiki_fields) : 0u;
        const bool reference_enabled = reference && (status_.feature_mask & bit);
        std::vector<panels::Row> wiki_rows;
        bool reference_ready = false;
        int enemy_rows = 0;
        if (available && !enemy_overview_.enemies().empty())
        {
            const auto& roster = enemy_overview_.enemies();
            const auto total = roster.size();
            enemy_rows = static_cast<int>(total);
            if (!view.selected_key.empty())
            {
                bool found = false;
                for (std::uint32_t index = 0; index < total; ++index)
                {
                    if (view.selected_key == roster[index].enemy_key)
                    {
                        view.selected = static_cast<int>(index);
                        found = true;
                        break;
                    }
                }
                if (!found) { view.selected = 0; }
            }
            if (view.selected >= enemy_rows)
            {
                view.selected = 0;
            }
            auto mask_color = [](std::uint32_t mask) -> unsigned {
                if ((mask & (1u << ipc::kDamageArts)) != 0)
                {
                    return static_cast<unsigned>(damage_type_color(2));
                }
                if ((mask & (1u << ipc::kDamagePhysical)) != 0)
                {
                    return static_cast<unsigned>(damage_type_color(1));
                }
                if ((mask & (1u << ipc::kDamageTrue)) != 0)
                {
                    return static_cast<unsigned>(damage_type_color(3));
                }
                if ((mask & (1u << ipc::kDamageElemental)) != 0)
                {
                    return static_cast<unsigned>(damage_type_color(5));
                }
                return static_cast<unsigned>(damage_type_color(4));
            };
            view.keys.clear();
            view.names.clear();
            for (int index = 0; index < enemy_rows; ++index)
            {
                const ipc::EnemyEntry& entry = roster[static_cast<std::size_t>(index)];
                panels::Row row;
                row.icon_only = (field_mask & kWikiAvatars) != 0;
                row.left = entry.enemy_name[0] != '\0' ? narrow_to_wide(entry.enemy_name)
                                                       : narrow_to_wide(entry.enemy_key);
                view.keys.emplace_back(entry.enemy_key);
                view.names.push_back(row.left);
                // 血量即生命上限，不重复占一行右侧；左侧并列头像+名字即可。
                row.accent = mask_color(entry.damage_type_mask);
                row.selected = index == view.selected;
                wiki_rows.push_back(std::move(row));
            }
            std::vector<HBITMAP> avatars;
            avatars.reserve(static_cast<std::size_t>(enemy_rows));
            if ((field_mask & kWikiAvatars) && !view.names.empty()) {
                const auto& name = view.names[static_cast<std::size_t>(view.selected)];
                if (!wiki_avatar_cache_.count(name) && !wiki_avatar_requested_.count(name) &&
                    now >= wiki_avatar_retry_[name] &&
                    prts_.request_image(name, wiki_avatar_failures_.count(name) != 0, true))
                    wiki_avatar_requested_[name] = true;
            }
            for (int index = 0; (field_mask & kWikiAvatars) != 0 && index < enemy_rows; ++index)
            {
                const ipc::EnemyEntry& entry = roster[static_cast<std::size_t>(index)];
                const std::wstring name = entry.enemy_name[0] != '\0' ? narrow_to_wide(entry.enemy_name)
                                                                     : narrow_to_wide(entry.enemy_key);
                const auto cached = wiki_avatar_cache_.find(name);
                if (cached != wiki_avatar_cache_.end())
                {
                    avatars.push_back(cached->second);
                }
                else
                {
                    avatars.push_back(nullptr);
                    if (wiki_avatar_requested_.find(name) == wiki_avatar_requested_.end() &&
                        now >= wiki_avatar_retry_[name])
                    {
                        if (prts_.request_image(name, wiki_avatar_failures_.count(name) != 0))
                            wiki_avatar_requested_[name] = true;
                    }
                }
            }
            panels::set_avatars(panel_id, avatars, 28);
            const ipc::EnemyEntry& selected = roster[static_cast<std::size_t>(view.selected)];
            view.selected_key = selected.enemy_key;
            auto stat_row = [&wiki_rows, field_mask](const wchar_t* label, const std::wstring& value) {
                const auto field = wiki_field_bit(label);
                if (field && !(field_mask & field)) return;
                panels::Row row;
                row.left = label;
                row.right = value;
                wiki_rows.push_back(std::move(row));
            };
            // 详情列表第一项：怪物名称（左侧图标列负责切换）。
            {
                const std::wstring selected_text =
                    selected.enemy_name[0] != '\0' ? narrow_to_wide(selected.enemy_name)
                                                    : narrow_to_wide(selected.enemy_key);
                stat_row(L"名称", selected_text);
            }
            if (overview_.header.battle_serial == snapshot.battle_serial &&
                (overview_.header.flags & ipc::kOverviewPartial)) {
                panels::Row note; note.wrap = true; note.left = L"部分关卡资料暂未读取完整";
                wiki_rows.push_back(std::move(note));
            }
            // Runtime and reference data belong to independent panels.
            if (!reference) {
                auto integer_stat = [&](const wchar_t* label, std::uint32_t bit, std::int64_t value) {
                    stat_row(label, (selected.stats_mask & bit) != 0 ? std::to_wstring(value) : L"—");
                };
                auto decimal_stat = [&](const wchar_t* label, std::uint32_t bit, std::int32_t value) {
                    wchar_t buffer[64] = {};
                    swprintf(buffer, std::size(buffer), L"%.2f", static_cast<double>(value) / 1000.0);
                    stat_row(label, (selected.stats_mask & bit) != 0 ? buffer : L"—");
                };
                integer_stat(L"生命上限", ipc::kEnemyMaxHp, selected.max_hp);
                integer_stat(L"攻击力", ipc::kEnemyAtk, selected.atk);
                integer_stat(L"防御力", ipc::kEnemyDef, selected.def);
                decimal_stat(L"法术抗性", ipc::kEnemyMagicResistance, selected.magic_resistance);
                decimal_stat(L"移动速度", ipc::kEnemyMoveSpeed, selected.move_speed);
                decimal_stat(L"攻击间隔", ipc::kEnemyBaseAttackTime, selected.base_attack_time);
                decimal_stat(L"重量等级", ipc::kEnemyWeight, selected.weight);
                integer_stat(L"阻挡数", ipc::kEnemyBlockCount, selected.block_cnt);
                integer_stat(L"漏怪数", ipc::kEnemyLifePointReduce, selected.life_point_reduce);
            }
            view.action_row = -1;
            if (reference_enabled)
            {
                const std::wstring selected_name = selected.enemy_name[0] != '\0'
                                                       ? narrow_to_wide(selected.enemy_name)
                                                       : narrow_to_wide(selected.enemy_key);
                if (wiki_name_ != selected_name)
                {
                    wiki_name_ = selected_name;
                    wiki_result_ = net::PrtsResult{};
                    wiki_request_ms_ = now;
                    prts_.request(selected_name);
                }
                std::wstring ready_name;
                net::PrtsResult ready;
                while (prts_.poll(ready_name, ready))
                {
                    if (ready_name != wiki_name_) continue;
                    wiki_result_ = ready;
                }
                if (!wiki_result_.ok && wiki_result_.error.empty() && wiki_request_ms_ &&
                    now - wiki_request_ms_ >= 15000) {
                    wiki_result_.error = L"请求超时，点击重试";
                }
                if (wiki_result_.ok)
                {
                    reference_ready = true;
                    if(wiki_result_.stale) {
                        panels::Row cached;
                        cached.left=L"PRTS 缓存资料（联网更新暂不可用）";
                        wiki_rows.push_back(std::move(cached));
                    }
                    // PRTS 内容按表格渲染：基础信息 + 各级别属性表；长文本随实际面板宽度重排。
                    auto push_header = [&wiki_rows](const std::wstring& text) {
                        panels::Row row;
                        row.header = true;
                        row.cells = {text};
                        wiki_rows.push_back(std::move(row));
                    };
                    auto push_table = [&wiki_rows](const std::vector<net::PrtsField>& fields) {
                        panels::Row row;
                        row.table = true;
                        for (const auto& field : fields) {
                            if (field.value.size() <= 10 && field.key.size() <= 6 && field.value.find(L'\n') == std::wstring::npos) {
                                row.cells.push_back(field.key);
                                row.cells.push_back(field.value);
                            }
                        }
                        if (!row.cells.empty()) wiki_rows.push_back(std::move(row));
                    };
                    auto push_field = [&wiki_rows](const std::wstring& key, const std::wstring& value) {
                        panels::Row row;
                        row.wrap = true;
                        row.left = key + L"：" + value;
                        wiki_rows.push_back(std::move(row));
                    };
                    auto is_short = [](const net::PrtsField& field) {
                        return field.value.size() <= 10 && field.key.size() <= 6 && field.value.find(L'\n') == std::wstring::npos;
                    };
                    std::vector<net::PrtsField> basic;
                    for (const auto& field : wiki_result_.basic)
                        if (wiki_prts_field_visible(field.key, field_mask, false)) basic.push_back(field);
                    if (!basic.empty())
                    {
                        push_header(L"PRTS 基础信息 · 参考资料");
                        push_table(basic);
                        for (const net::PrtsField& field : basic)
                        {
                            if (!is_short(field))
                            {
                                push_field(field.key, field.value);
                            }
                        }
                    }
                    for (const net::PrtsLevel& level : wiki_result_.levels)
                    {
                        std::vector<net::PrtsField> fields;
                        for (const auto& field : level.fields)
                            if (wiki_prts_field_visible(field.key, field_mask, true)) fields.push_back(field);
                        if (fields.empty()) continue;
                        push_header(L"PRTS 级别 " + std::to_wstring(level.index) + L" · 参考数值");
                        push_table(fields);
                        for (const net::PrtsField& field : fields)
                        {
                            if (!is_short(field))
                            {
                                push_field(field.key, field.value);
                            }
                        }
                    }
                }
                else if (!wiki_result_.error.empty())
                {
                    panels::Row row;
                    row.left = L"PRTS";
                    row.right = wiki_result_.error;
                    wiki_rows.push_back(std::move(row));
                }
                view.action_row = static_cast<int>(wiki_rows.size());
                panels::Row action;
                action.left = wiki_result_.ok ? L"刷新资料" : !wiki_result_.error.empty() ? L"重试查询" : L"加载中…";
                action.accent = static_cast<unsigned>(theme::kAccent);
                wiki_rows.push_back(std::move(action));

            }
        }
        else
        {
            view.action_row = -1;
            panels::set_avatars(panel_id, {}, 28);
            wiki_rows.push_back(panels::Row{panel_session_.retained() ? L"本场没有敌人目录" : L"等待当前关卡敌人目录…", L"", 0});
        }
        view.enemy_rows = enemy_rows;
        if (view.auto_width.take(available && (status_.feature_mask & bit) != 0 &&
                                 (!reference || reference_ready))) {
            if (reference) panels::fit_reference_width(panel_id, wiki_rows);
            else panels::fit_width(panel_id, 300);
        }
        panels::update(panel_id, reference ? L"敌方 PRTS" : L"敌方数据", wiki_rows,
                       available && panel_session_.visible(bit) &&
                       (status_.feature_mask & bit) != 0);
    }
}

void App::refresh_game_process()
{
    std::wstring found;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry) != FALSE)
        {
            do
            {
                if (equals_ignore_case_prefix(entry.szExeFile, L"arknights"))
                {
                    found = entry.szExeFile;
                    break;
                }
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }
        CloseHandle(snapshot);
    }

    // 受限上下文（如从沙箱执行器启动）里 Toolhelp 可能看不到其他会话的进程，
    // 此时用窗口枚举兜底：能找到游戏主窗口就视为游戏在运行。
    if (found.empty())
    {
        GameWindowSearch search = {};
        EnumWindows(enum_game_window, reinterpret_cast<LPARAM>(&search));
        if (search.window != nullptr)
        {
            found = L"Arknights.exe";
            DWORD process_id = 0;
            GetWindowThreadProcessId(search.window, &process_id);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
            if (process != nullptr)
            {
                wchar_t path[1024] = {};
                DWORD length = static_cast<DWORD>(std::size(path));
                if (QueryFullProcessImageNameW(process, 0, path, &length) != FALSE)
                {
                    const wchar_t* name = std::wcsrchr(path, L'\\');
                    if (name != nullptr && name[1] != L'\0')
                    {
                        found = name + 1;
                    }
                }
                CloseHandle(process);
            }
        }
    }

    if (found != status_.game_process)
    {
        status_.game_process = found;
        if (found.empty())
        {
            log_info("game process not found");
        }
        else
        {
            log_info("game process detected: %s", wide_to_utf8(found).c_str());
        }
    }
}

void App::refresh_link_state()
{
    // 只有在 provider（插件侧发布层）确实存在时才进入「连接中/等待战斗」；
    // 仅仅检测到游戏窗口并不等于已连接——那种情况应保持「未加载运行时」提示。
    LinkState next = LinkState::kDisconnected;
    if (status_.battle_valid && status_.in_battle && status_.battle_state != 3)
    {
        next = LinkState::kBattle;
    }
    else if (status_.battle_valid || status_.battle_mapping_open)
    {
        next = LinkState::kWaiting;
    }

    if (next != status_.link)
    {
        status_.link = next;
        log_info("link state -> %s", wide_to_utf8(link_state_label(next)).c_str());
        ball_set_link_state(next);
        refresh_tray_tooltip();
    }
}

std::wstring format_link_tooltip(LinkState state)
{
    std::wstring text = constants::kAppTitle;
    text += L" · ";
    text += link_state_label(state);
    return text;
}

void App::refresh_tray_tooltip()
{
    if (!tray_active_)
    {
        return;
    }
    const std::wstring tooltip = format_link_tooltip(status_.link);
    tray_set_tooltip(tooltip.c_str());
}

void App::request_launch()
{
    if (game_launch::busy())
    {
        log_info("launch ignored: launcher is busy");
        return;
    }
    if (!game_launch::paths_ready())
    {
        log_warn("launch requested but paths unresolved: %s", wide_to_utf8(game_launch::last_error()).c_str());
        std::wstring message = L"无法定位游戏或 ArkHelper 启动组件。\n\n";
        message += game_launch::last_error();
        message += L"\n\n请检查 config\\arkhelp.json 里的 game_path，"
                   L"并保持 ArkHelper 程序文件夹完整。";
        MessageBoxW(nullptr, message.c_str(), constants::kAppTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    const game_launch::Paths& target = game_launch::paths();
    log_info("launch requested: game=%s loader=%s", wide_to_utf8(target.game_exe).c_str(),
             wide_to_utf8(target.loader_exe).c_str());
    if (!game_launch::request_launch())
    {
        const std::wstring& error = game_launch::last_error();
        if (error.rfind(L"已取消", 0) == 0)
        {
            log_info("launch cancelled by user");
            return;
        }
        std::wstring message = L"启动游戏失败。\n\n";
        message += error;
        MessageBoxW(nullptr, message.c_str(), constants::kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }
    // The installed agent starts once or connects to an existing game; it never closes it.
    auto_launch_attempted_ = true;
    log_info("launcher started, waiting for game and provider");
}

void App::show_menu()
{
    if (quitting_ || shell_hwnd_ == nullptr)
    {
        return;
    }
    if (menu_is_visible())
    {
        menu_hide();
        return;
    }
    RECT anchor{0, 0, 0, 0};
    const bool had_ball = ball_is_visible() && ball_get_rect(anchor);
    if (!had_ball)
    {
        // 悬浮球被隐藏时没有锚点，退化为屏幕工作区中心，菜单仍然能被托盘唤出。
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int center_x = (work.left + work.right) / 2;
        const int center_y = (work.top + work.bottom) / 2;
        anchor = RECT{center_x, center_y, center_x + 1, center_y + 1};
    }
    // 点击球后球消失、菜单出现在原球位置；关闭菜单时球再出现。
    if (had_ball)
    {
        ball_show(false);
    }
    menu_show(config_, anchor);
    wchar_t debug_config[8] = {};
    if (GetEnvironmentVariableW(L"ARKHELP_DEBUG_CONFIG", debug_config, 8) > 0 && debug_config[0] != L'0')
    {
        menu_open_config(ipc::kFeatureDamageNumbers);
    }
}

void App::show_settings()
{
    if (quitting_)
    {
        return;
    }
    if (!menu_is_visible()) show_menu();
    menu_open_settings();
}

void App::toggle_ball()
{
    const bool visible = ball_is_visible();
    ball_show(!visible);
    if (visible)
    {
        // 球隐藏后菜单失去锚点，顺手关掉避免菜单飘在屏幕中央。
        menu_hide();
    }
    save_config();
    log_info("ball visible=%d", static_cast<int>(!visible));
}

void App::on_ball_drag_finished(int x, int y, int edge)
{
    config_.ball_x = x;
    config_.ball_y = y;
    config_.edge = edge;
    save_config();
    log_info("ball moved to (%d,%d) edge=%d", x, y, edge);
}

void App::on_feature_toggle(uint32_t bit, bool enabled)
{
    const uint32_t base_mask = commands_.ready() ? commands_.feature_mask() : config_.features;
    const uint32_t mask = (enabled ? (base_mask | bit) : (base_mask & ~bit)) & config_.menu_features;
    config_.features = mask;
    status_.feature_mask = mask;
    if (enabled && bit == ipc::kFeatureDamageNumbers) damage_width_revision_ = {};
    if (commands_.ready())
    {
        commands_.set_feature_mask(mask);
        status_.revision = commands_.revision();
    }
    else
    {
        log_warn("feature toggle without command mapping, config only");
    }
    save_config();
    menu_refresh();
    log_info("feature mask -> 0x%08X (bit=0x%08X enabled=%d revision=%lu)", static_cast<unsigned>(mask),
             static_cast<unsigned>(bit), static_cast<int>(enabled), static_cast<unsigned long>(status_.revision));
}

void App::on_opacity_changed(int percent, bool commit)
{
    config_.opacity = std::clamp(percent, 40, 100);
    ball_set_opacity(config_.opacity);
    if (commit)
    {
        save_config();
        log_info("opacity -> %d%%", config_.opacity);
    }
}

void App::on_ui_scale_changed(int percent)
{
    config_.ui_scale = percent;
    if (commands_.ready())
    {
        commands_.set_ui_scale(static_cast<float>(percent) / 100.0f);
    }
    // 悬浮球也要跟着缩放，否则设置面板看起来"没反应"。
    ball_set_scale(config_.ui_scale);
    panels::refresh_theme(config_.ui_scale);
    save_config();
    menu_sync_config(config_);
    menu_refresh();
    log_info("ui scale -> %d%%", percent);
}

void App::apply_appearances()
{
    panels::set_statistics_compact(config_.damage_compact);
    for (const auto bit : kAppearanceFeatures)
        overlay::set_appearance(bit, feature_appearance(config_, bit));
    panels::set_appearance(panels::kDamagePanelId, feature_appearance(config_, ipc::kFeatureDamageNumbers));
    panels::set_appearance(panels::kSpawnPanelId, feature_appearance(config_, ipc::kFeatureSpawnList));
    panels::set_appearance(panels::kWikiPanelId, feature_appearance(config_, ipc::kFeatureWiki));
    panels::set_appearance(panels::kPrtsPanelId, feature_appearance(config_, ipc::kFeatureEnemyPrts));
}

void App::on_reset_defaults()
{
    // 重置仅恢复外观/功能开关，不抹掉一键启动需要的游戏与加载器路径。
    const std::wstring game_path = config_.game_path;
    const std::wstring loader_path = config_.loader_path;
    config_ = config_defaults();
    config_.game_path = game_path;
    config_.loader_path = loader_path;
    overlay::set_options(config_.overlay);
    configure_prts_cache();
    if (commands_.ready())
    {
        commands_.set_feature_mask(config_.features);
        // 重置也要把缩放写回命令区，否则 provider 侧还按旧缩放画叠加层。
        commands_.set_ui_scale(static_cast<float>(config_.ui_scale) / 100.0f);
        status_.revision = commands_.revision();
    }
    status_.feature_mask = config_.features;
    overlay::set_widget_config(config_.hp_only_paused, config_.spawn_only_paused, config_.range_only_paused);
    overlay::set_damage_config(config_.damage_metric_mask, config_.damage_merged,
                               config_.damage_sort_metric, config_.damage_order);
    ball_set_opacity(config_.opacity);
    ball_set_scale(config_.ui_scale);
    panels::restore_geometry(panels::kDamagePanelId, config_.damage_panel_x, config_.damage_panel_y,
                             config_.damage_panel_collapsed);
    panels::restore_geometry(panels::kSpawnPanelId, config_.spawn_panel_x, config_.spawn_panel_y,
                             config_.spawn_panel_collapsed);
    panels::restore_height(panels::kDamagePanelId, 0);
    panels::restore_height(panels::kSpawnPanelId, 0);
    panels::restore_height(panels::kWikiPanelId, 0);
    panels::restore_height(panels::kPrtsPanelId, 0);
    panels::restore_width(panels::kDamagePanelId, 0);
    panels::restore_width(panels::kSpawnPanelId, 0);
    panels::restore_width(panels::kWikiPanelId, 0);
    panels::restore_width(panels::kPrtsPanelId, 0);
    enemy_views_[0].auto_width = {};
    enemy_views_[1].auto_width = {};
    panels::refresh_theme(config_.ui_scale);
    apply_appearances();
    save_config();
    menu_sync_config(config_);
    menu_refresh();
    log_info("settings reset to defaults (launch paths preserved)");
}

void App::on_exit_requested()
{
    if (quitting_)
    {
        return;
    }
    quitting_ = true;
    log_info("exit requested");
    save_config();
    menu_hide();
    ball_show(false);
    if (tray_active_)
    {
        tray_destroy();
        tray_active_ = false;
    }
    if (shell_hwnd_ != nullptr)
    {
        // 销毁协调窗口 → WM_DESTROY → PostQuitMessage，消息循环自然退出。
        DestroyWindow(shell_hwnd_);
    }
}

void App::save_config()
{
    config_.dock_state = panels::save_dock_state();
    log_info("save_config dock_state=%s", wide_to_utf8(config_.dock_state).c_str());
    if (config_path_.empty())
    {
        return;
    }
    if (!config_save(config_path_, config_))
    {
        log_warn("config save failed: %s", wide_to_utf8(config_path_).c_str());
    }
}

void App::sync_commands_from_config()
{
    if (!commands_.ready())
    {
        return;
    }
    commands_.set_feature_mask(config_.features);
    commands_.set_ui_scale(static_cast<float>(config_.ui_scale) / 100.0f);
}

LRESULT CALLBACK App::shell_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return App::instance().handle_shell_message(hwnd, message, wparam, lparam);
}

LRESULT App::handle_shell_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_TIMER:
        if (wparam == constants::kPollTimerId)
        {
            poll();
            return 0;
        }
        break;
    case constants::kTrayCallbackMessage:
        if (tray_handle_message(wparam, lparam))
        {
            return 0;
        }
        break;
    case constants::kMsgWake:
        log_info("wake request from second instance");
        show_menu();
        return 0;
    case constants::kMsgShowMenu:
        show_menu();
        return 0;
    case constants::kMsgShowSettings:
        show_settings();
        return 0;
    case constants::kMsgToggleBall:
        toggle_ball();
        return 0;
    case constants::kMsgExit:
        on_exit_requested();
        return 0;
    case WM_CLOSE:
        on_exit_requested();
        return 0;
    case WM_DESTROY:
        shell_hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int run_app(HINSTANCE instance)
{
    // Per-Monitor V2：Win11 150% 缩放下，分层窗口尺寸、字体与命中区域都要以真实 DPI 计算。
    // 老系统没有该 API 时 SetProcessDpiAwarenessContext 返回 FALSE，程序继续运行，仅缩放可能被系统拉伸。
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE)
    {
        // 不写日志：日志尚未初始化，且这里失败不影响最小闭环。
    }

    App& app = App::instance();
    const int init_result = app.initialize(instance);
    if (init_result == 1)
    {
        // 已有实例：第二实例已完成唤起，直接以成功码退出。
        return 0;
    }
    if (init_result != 0)
    {
        app.shutdown();
        return init_result == 10 ? 0 : init_result;
    }
    return app.run();
}

}  // namespace arkhelp
