// Development harness: exercise the real renderer and hit testing in the same TU.
#include "../src/ui/menu.cpp"
#include "../src/ui/panels.cpp"
#include "ui_review_bitmap.h"
#include <objbase.h>
#include <commctrl.h>
#include <cstdio>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    using namespace arkhelp;
    const auto destination = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"build/ui-review-current");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        Config config;
        if(config.feature_order!=kDefaultFeatureOrder || config.feature_order.front()!=ipc::kFeatureDamageNumbers || config.menu_height!=572)
            throw std::runtime_error("default feature order or 30 percent taller menu missing");
        config.damage_metric_mask = statistics::kAllMetrics;
        Status status;
        status.command_mapping = true;
        status.feature_mask = config.features;
        MenuCallbacks callbacks;
        int cache_opened=0;
        callbacks.get_config = [&] { return config; };
        callbacks.get_status = [&]() -> const Status& { return status; };
        callbacks.get_feature_mask = [&] { return config.features; };
        callbacks.on_feature_toggle = [&](std::uint32_t bit, bool enabled) {
            config.features = enabled ? config.features | bit : config.features & ~bit;
        };
        callbacks.get_only_paused = [&](std::uint32_t bit) {
            return bit == ipc::kFeatureEnemyRange ? config.range_only_paused :
                bit == ipc::kFeatureHpBars ? config.hp_only_paused : config.spawn_only_paused;
        };
        callbacks.on_only_paused = [&](std::uint32_t bit, bool enabled) {
            if (bit == ipc::kFeatureEnemyRange) config.range_only_paused = enabled;
            else if (bit == ipc::kFeatureHpBars) config.hp_only_paused = enabled;
            else config.spawn_only_paused = enabled;
        };
        callbacks.get_damage_config = [&] {
            DamageConfig result;
            result.metric_mask = config.damage_metric_mask;
            result.order = config.damage_order;
            result.compact = config.damage_compact;
            result.show_devices = config.damage_show_devices;
            return result;
        };
        callbacks.on_damage_config = [&](const DamageConfig& value) {
            config.damage_metric_mask=value.metric_mask; config.damage_order=value.order;
            config.damage_merged=value.merged; config.damage_sort_metric=value.sort_metric;
            config.damage_compact=value.compact;
            config.damage_show_devices=value.show_devices;
        };
        callbacks.on_appearance = [&](std::uint32_t bit,const FeatureAppearance& value) {
            feature_appearance(config,bit)=value;
        };
        callbacks.on_wiki_fields = [&](std::uint32_t bit, std::uint32_t value) {
            (bit == ipc::kFeatureEnemyPrts ? config.prts_fields : config.wiki_fields) = value;
        };
        callbacks.on_feature_menu = [&](std::uint32_t mask, const FeatureOrder& order) {
            config.menu_features=mask; config.feature_order=order; config.features &= mask;
        };
        callbacks.on_menu_opacity = [&](int value) { config.menu_opacity=value; };
        callbacks.on_menu_height = [&](int value) { config.menu_height=value; };
        callbacks.on_auto_launch = [&](bool value) { config.auto_launch=value; };
        callbacks.on_overlay_options = [&](const OverlayOptions& value) { config.overlay=value; };
        callbacks.on_prts_cache_limit = [&](int value) { config.prts_cache_mb=value; };
        callbacks.get_prts_cache_path = [&] { return config.prts_cache_path.empty()?L"D:\\ArkHelper\\cache\\prts":config.prts_cache_path; };
        callbacks.on_prts_cache_path = [&](const std::wstring& value) -> std::wstring {
            if(value==L"unwritable") return L"该目录无法写入，已保留原缓存路径";
            config.prts_cache_path=value;return {};
        };
        callbacks.on_prts_cache_open = [&]() -> std::wstring { ++cache_opened;return {}; };
        callbacks.on_opacity = [&](int value, bool) { config.opacity = value; };
        callbacks.on_ui_scale = [&](int value) { config.ui_scale = value; menu_sync_config(config); };
        callbacks.on_reset = [&] { config = Config{}; menu_sync_config(config); };
        if (!menu_create(GetModuleHandleW(nullptr), callbacks)) throw std::runtime_error("menu creation failed");
        g_menu.dpi = 144;
        g_menu.work = RECT{0, 0, 1920, 1040};
        g_menu.ui_scale = 100;
        load_menu_logo();
        const auto save = [&](const wchar_t* file) { render(); save_review_bitmap(g_menu.surface, destination / file); };
        const auto click_rect = [&](RECT rect,HitTarget expected) {
            const auto id=static_cast<int>(expected);
            if ((id>=140 && id<=152) || (id>=300 && id<316) || (id>=500 && id<510)) rect=row_switch(rect);
            if (expected==HitTarget::kConfigToggle) rect=g_menu.layout.pause_switch;
            if (expected==HitTarget::kAutoLaunch) rect=g_menu.layout.auto_launch_switch;
            for (const auto& control:extra_controls()) if(control.target==expected && control.maximum==1) rect=control.track;
            const int center=(rect.top+rect.bottom)/2;
            g_menu.viewport.scroll=std::clamp(center-(g_menu.viewport.top+g_menu.viewport.bottom)/2,
                                             0,g_menu.viewport.maximum_scroll());
            render();
            const POINT point{(rect.left+rect.right)/2,center-g_menu.viewport.scroll};
            if(hit_test(point)!=expected) throw std::runtime_error("scrolled control hit a different target");
            SendMessageW(g_menu.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(point.x,point.y));
            SendMessageW(g_menu.hwnd,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
        };
        open_page(0);
        if(g_menu.layout.toggle_rows[2].top>=g_menu.layout.toggle_rows[0].top)
            throw std::runtime_error("default menu did not start with statistics");
        save(L"menu-default-order.bmp");
        auto height_grip=menu_height_grip();
        POINT height_point{(height_grip.left+height_grip.right)/2,(height_grip.top+height_grip.bottom)/2};
        SendMessageW(g_menu.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(height_point.x,height_point.y));
        if(!g_menu.resizing_height) throw std::runtime_error("main menu bottom edge did not start height resize");
        g_menu.resize_cursor_start.y-=S(60);
        SendMessageW(g_menu.hwnd,WM_MOUSEMOVE,MK_LBUTTON,0);
        SendMessageW(g_menu.hwnd,WM_LBUTTONUP,0,0);
        if(config.menu_height!=632 || g_menu.resizing_height) throw std::runtime_error("main menu height was not saved on release");
        save(L"menu-main-resized.bmp");
        height_grip=menu_height_grip();height_point={(height_grip.left+height_grip.right)/2,(height_grip.top+height_grip.bottom)/2};
        SendMessageW(g_menu.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(height_point.x,height_point.y));
        g_menu.resize_cursor_start.y+=S(900);
        SendMessageW(g_menu.hwnd,WM_MOUSEMOVE,MK_LBUTTON,0);SendMessageW(g_menu.hwnd,WM_LBUTTONUP,0,0);
        if(config.menu_height!=400) throw std::runtime_error("main menu can be resized below its usable minimum");
        config.menu_height=572;config.feature_order=kAppearanceFeatures;menu_sync_config(config);open_page(0);
        save(L"menu-main.bmp");
        const int main_height=g_menu.surface.height;
        status.launch_error=L"未找到游戏路径，请在游戏启动后重试连接。";
        save(L"menu-launch-error.bmp");
        if(g_menu.surface.height!=main_height) throw std::runtime_error("launch error changed menu height");
        status.launch_error.clear();
        const HWND original = g_menu.hwnd;
        menu_open_config(ipc::kFeatureHpBars);
        save(L"menu-hp-paused.bmp");
        if (g_menu.viewport.content_height != S(kAppearanceHeight+264) || g_menu.surface.height>S(464))
            throw std::runtime_error("appearance and pause configuration retained excessive blank height");
        for(const auto bit:kAppearanceFeatures) {
            menu_open_config(bit);
            const auto& area=g_menu.layout.appearance_font;
            const POINT point{(area.left+area.right)/2,(area.top+area.bottom)/2};
            if(bit==ipc::kFeatureEnemyRange) {
                if(hit_test(point)==HitTarget::kAppearanceFont || hit_test(point)==HitTarget::kAppearanceColor ||
                   g_menu.viewport.content_height!=S(130+200))
                    throw std::runtime_error("range exposes unused text options or keeps their blank space");
            } else if(bit==ipc::kFeatureEnemyPaths) {
                if(hit_test(point)!=HitTarget::kAppearanceColor) throw std::runtime_error("path colour control missing");
            } else if(hit_test(point)!=HitTarget::kAppearanceFont)
                throw std::runtime_error("text feature is missing font configuration");
            const RECT slider=g_menu.layout.appearance_opacity_track;
            SendMessageW(g_menu.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM((slider.left+slider.right)/2,(slider.top+slider.bottom)/2));
            SendMessageW(g_menu.hwnd,WM_LBUTTONUP,0,0);
            if(feature_appearance(config,bit).opacity<59 || feature_appearance(config,bit).opacity>60)
                throw std::runtime_error("feature opacity slider did not apply independently");
            if(bit!=ipc::kFeatureEnemyRange && bit!=ipc::kFeatureEnemyPaths) {
                g_menu.appearance.font_size=32;
                g_menu.appearance.text_color=0x12ABCD;
                apply_appearance();
                if(feature_appearance(config,bit).font_size!=32 || feature_appearance(config,bit).text_color!=0x12ABCD)
                    throw std::runtime_error("feature font or RGB configuration was not passed to App");
            }
            click_rect(g_menu.layout.appearance_reset,HitTarget::kAppearanceReset);
            if(!(feature_appearance(config,bit)==FeatureAppearance{}))
                throw std::runtime_error("feature appearance default did not reset");
        }
        menu_open_config(ipc::kFeatureEnemyRange);
        click_rect(g_menu.layout.pause_row,HitTarget::kConfigToggle);
        if (!config.range_only_paused || config.hp_only_paused) throw std::runtime_error("range pause setting changed the wrong feature");
        save(L"menu-range-paused.bmp");
        const auto extra_rect=[&](HitTarget target) {
            for(const auto& control:extra_controls()) if(control.target==target)
                return control.maximum==1?control.row:control.track;
            throw std::runtime_error("extra control not laid out");
        };
        for(auto bit:{ipc::kFeatureEnemyPaths,ipc::kFeatureEnemyRange}) {
            menu_open_config(bit);
            click_rect(extra_rect(HitTarget::kLineWidth),HitTarget::kLineWidth);
            click_rect(line_style_rect(2),static_cast<HitTarget>(static_cast<int>(HitTarget::kLineStyleFirst)+2));
            if(feature_appearance(config,bit).line_width!=5 || feature_appearance(config,bit).line_style!=2)
                throw std::runtime_error("line width or dot style changed the wrong appearance");
            save(bit==ipc::kFeatureEnemyPaths?L"menu-path-line-style.bmp":L"menu-range-line-style.bmp");
            click_rect(g_menu.layout.appearance_reset,HitTarget::kAppearanceReset);
            if(feature_appearance(config,bit).line_width!=2 || feature_appearance(config,bit).line_style!=0)
                throw std::runtime_error("line appearance defaults were not restored");
        }
        menu_open_config(ipc::kFeatureEnemyStats);
        click_rect(extra_rect(HitTarget::kEnemyPause),HitTarget::kEnemyPause);
        click_rect(extra_rect(HitTarget::kEnemyX),HitTarget::kEnemyX);
        if(!config.overlay.enemy_only_paused || config.overlay.enemy_offset_x!=0)
            throw std::runtime_error("enemy pause or offset settings were not applied");
        config.overlay.enemy_metric_mask=0x3f;menu_sync_config(config);
        click_rect(damage_row_rect(g_menu.layout,6),static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+6));
        if(config.overlay.enemy_metric_mask!=0x3f) throw std::runtime_error("seventh enemy metric was allowed");
        click_rect(damage_row_rect(g_menu.layout,0),HitTarget::kDamageToggleFirst);
        click_rect(damage_row_rect(g_menu.layout,6),static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+6));
        if(std::popcount(config.overlay.enemy_metric_mask)!=6 || !(config.overlay.enemy_metric_mask&ipc::kEnemyAttackSpeed))
            throw std::runtime_error("enemy metric replacement failed");
        auto enemy_row=damage_row_rect(g_menu.layout,6);
        click_rect(RECT{enemy_row.right-S(126),enemy_row.top+S(2),enemy_row.right-S(100),enemy_row.bottom-S(2)},
            static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageUpFirst)+6));
        if(enemy_metrics::parse_order(config.overlay.enemy_order)[5]!=6)
            throw std::runtime_error("enemy attribute order did not persist");
        save(L"menu-enemy-metrics.bmp");
        g_menu.viewport.scroll=0;save(L"menu-enemy-options.bmp");
        menu_open_config(ipc::kFeatureHpBars);
        click_rect(extra_rect(HitTarget::kHpHover),HitTarget::kHpHover);
        if(!config.overlay.hp_only_hover) throw std::runtime_error("HP hover switch did not apply");
        click_rect(extra_rect(HitTarget::kHpY),HitTarget::kHpY);
        if(config.overlay.hp_offset_y!=0) throw std::runtime_error("signed HP offset did not reach center zero");
        save(L"menu-hp-offsets.bmp");
        menu_open_config(ipc::kFeatureOperatorStats);
        save(L"menu-operator-top.bmp");
        click_rect(extra_rect(HitTarget::kOperatorPause),HitTarget::kOperatorPause);
        click_rect(extra_rect(HitTarget::kOperatorHover),HitTarget::kOperatorHover);
        if(config.overlay.operator_show_devices || config.damage_show_devices) throw std::runtime_error("devices must be hidden by default");
        click_rect(extra_rect(HitTarget::kOperatorDevices),HitTarget::kOperatorDevices);
        if(!config.overlay.operator_show_devices || config.damage_show_devices) throw std::runtime_error("operator device switch must be independent of statistics");
        if(!config.overlay.operator_only_paused || !config.overlay.operator_only_hover)
            throw std::runtime_error("operator pause/hover settings failed");
        config.overlay.operator_metric_mask=0x3f;
        menu_sync_config(config);
        click_rect(damage_row_rect(g_menu.layout,6),static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+6));
        if(config.overlay.operator_metric_mask!=0x3f) throw std::runtime_error("seventh metric was allowed");
        click_rect(damage_row_rect(g_menu.layout,0),HitTarget::kDamageToggleFirst);
        click_rect(damage_row_rect(g_menu.layout,6),static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+6));
        if(std::popcount(config.overlay.operator_metric_mask)!=6 || !(config.overlay.operator_metric_mask&(1u<<6)))
            throw std::runtime_error("operator metric replacement failed");
        save(L"menu-operator-metrics.bmp");
        menu_open_config(ipc::kFeatureEnemyPaths);
        click_rect(extra_rect(HitTarget::kPathClick),HitTarget::kPathClick);
        click_rect(extra_rect(HitTarget::kPathSpeed),HitTarget::kPathSpeed);
        click_rect(extra_rect(HitTarget::kPathLinger),HitTarget::kPathLinger);
        if(!config.overlay.path_on_click || config.overlay.path_speed<1000 || config.overlay.path_linger_ms!=5000)
            throw std::runtime_error("path animation options hit wrong controls");
        save(L"menu-path-options.bmp");
        menu_open_settings();
        save(L"menu-settings.bmp");
        if (g_menu.hwnd != original) throw std::runtime_error("settings opened a separate window");
        const bool original_auto=config.auto_launch;
        click_rect(g_menu.layout.auto_launch_row,HitTarget::kAutoLaunch);
        if(config.auto_launch==original_auto) throw std::runtime_error("auto-launch setting did not change");
        const RECT menu_slider=g_menu.layout.menu_opacity_track;
        click_rect(menu_slider,HitTarget::kMenuOpacity);
        if(config.menu_opacity<59 || config.menu_opacity>60 || config.opacity!=100)
            throw std::runtime_error("menu opacity changed the wrong appearance");
        const RECT ball_slider=g_menu.layout.opacity_track;
        click_rect(ball_slider,HitTarget::kOpacity);
        if(config.opacity<69 || config.opacity>70) throw std::runtime_error("ball opacity slider failed");
        click_rect(extra_rect(HitTarget::kCacheLimit),HitTarget::kCacheLimit);
        if(config.prts_cache_mb<2500 || config.prts_cache_mb>2600) throw std::runtime_error("cache budget slider failed");
        save(L"menu-cache-limit.bmp");
        commit_cache_path(L"E:\\我的 PRTS 缓存\\ArkHelper");
        if(config.prts_cache_path!=g_menu.cache_path || !g_menu.cache_error.empty())
            throw std::runtime_error("cache path change did not update settings");
        commit_cache_path(L"unwritable");
        if(g_menu.cache_error.empty() || config.prts_cache_path!=g_menu.cache_path)
            throw std::runtime_error("failed directory changed the current cache path");
        click_rect(cache_path_button(0),HitTarget::kCacheOpen);
        if(cache_opened!=1) throw std::runtime_error("cache folder open button missed its callback");
        save(L"menu-cache-path.bmp");
        click_rect(cache_path_button(2),HitTarget::kCacheDefault);
        if(!config.prts_cache_path.empty() || g_menu.cache_path!=L"D:\\ArkHelper\\cache\\prts")
            throw std::runtime_error("cache directory default failed");
        const auto change=cache_path_button(1);
        g_menu.viewport.scroll=std::clamp<int>((change.top+change.bottom)/2-(g_menu.viewport.top+g_menu.viewport.bottom)/2,0,g_menu.viewport.maximum_scroll());
        if(hit_test({(change.left+change.right)/2,(change.top+change.bottom)/2-g_menu.viewport.scroll})!=HitTarget::kCacheChange)
            throw std::runtime_error("change cache folder button is unreachable");
        menu_open_config(ipc::kFeatureWiki);
        save(L"menu-wiki-fields-top.bmp");
        const auto old_mask=config.wiki_fields;
        for(const auto& field:wiki_page_fields()) if(field.bit==kWikiSource) throw std::runtime_error("redundant data source setting remains");
        const auto wiki_options=wiki_page_fields();
        const int avatar_index=static_cast<int>(std::find_if(wiki_options.begin(),wiki_options.end(),[](const auto& field){return field.bit==kWikiAvatars;})-wiki_options.begin());
        click_rect(wiki_field_rect(avatar_index),static_cast<HitTarget>(static_cast<int>(HitTarget::kWikiFieldFirst)+avatar_index));
        if(config.wiki_fields!=(old_mask^kWikiAvatars)) throw std::runtime_error("WIKI scrolled field changed wrong mask");
        save(L"menu-wiki-fields-bottom.bmp");
        click_rect(wiki_fields_button(true),HitTarget::kWikiFieldsDefault);
        if(config.wiki_fields!=kWikiAllFields) throw std::runtime_error("WIKI field defaults failed");
        menu_open_config(ipc::kFeatureEnemyPrts);
        const auto old_prts=config.prts_fields;
        const auto prts_options=wiki_page_fields();
        const int prts_stats_index=static_cast<int>(std::find_if(prts_options.begin(),prts_options.end(),[](const auto& field){return field.bit==kWikiPrtsStats;})-prts_options.begin());
        click_rect(wiki_field_rect(prts_stats_index),static_cast<HitTarget>(static_cast<int>(HitTarget::kWikiFieldFirst)+prts_stats_index));
        if(config.prts_fields!=(old_prts^kWikiPrtsStats) || config.wiki_fields!=kWikiAllFields)
            throw std::runtime_error("PRTS preferences changed runtime data settings");
        save(L"menu-prts-fields.bmp");
        open_page(0);
        const auto initial_features=config.features;
        const auto label_row=g_menu.layout.toggle_rows[0];
        POINT label{label_row.left+S(16),(label_row.top+label_row.bottom)/2};
        label.y-=g_menu.viewport.scroll;
        SendMessageW(g_menu.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(label.x,label.y));
        SendMessageW(g_menu.hwnd,WM_LBUTTONUP,0,MAKELPARAM(label.x,label.y));
        if(config.features!=initial_features) throw std::runtime_error("feature label toggled the switch");
        const auto sw=g_menu.layout.toggle_switches[0];
        POINT press{(sw.left+sw.right)/2,(sw.top+sw.bottom)/2-g_menu.viewport.scroll};
        SendMessageW(g_menu.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(press.x,press.y));
        if(config.features!=initial_features) throw std::runtime_error("switch changed before a completed click");
        SendMessageW(g_menu.hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(press.x-S(20),press.y));
        SendMessageW(g_menu.hwnd,WM_LBUTTONUP,0,MAKELPARAM(press.x,press.y));
        if(config.features!=initial_features) throw std::runtime_error("drag cancellation still toggled a switch");
        click_rect(sw,HitTarget::kToggleFirst);
        if(config.features!=(initial_features^ipc::kFeatureSpawnTimer)) throw std::runtime_error("real switch click failed");
        menu_open_settings();
        auto cache=extra_rect(HitTarget::kCacheLimit);
        g_menu.viewport.scroll=std::clamp<int>((cache.top+cache.bottom)/2-(g_menu.viewport.top+g_menu.viewport.bottom)/2,0,g_menu.viewport.maximum_scroll());
        render();
        POINT wheel{(cache.left+cache.right)/2,(cache.top+cache.bottom)/2-g_menu.viewport.scroll};
        ClientToScreen(g_menu.hwnd,&wheel);
        const auto cache_before=config.prts_cache_mb;
        const int scroll_before=g_menu.viewport.scroll;
        SendMessageW(g_menu.hwnd,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),MAKELPARAM(wheel.x,wheel.y));
        if(config.prts_cache_mb!=cache_before || g_menu.viewport.scroll<=scroll_before)
            throw std::runtime_error("wheel over a slider adjusted its value instead of scrolling");
        click_rect(managed_feature_row(0),HitTarget::kManageToggleFirst);
        if(config.menu_features&ipc::kFeatureSpawnTimer || config.features&ipc::kFeatureSpawnTimer)
            throw std::runtime_error("hiding a menu feature did not disable it");
        auto manage=managed_feature_row(0);
        click_rect(RECT{manage.right-S(80),manage.top+S(3),manage.right-S(54),manage.bottom-S(3)},HitTarget::kManageDownFirst);
        if(config.feature_order[0]!=ipc::kFeatureSpawnList || config.feature_order[1]!=ipc::kFeatureSpawnTimer)
            throw std::runtime_error("feature menu order was not updated");
        save(L"menu-feature-management.bmp");
        open_page(0);
        if(!IsRectEmpty(&g_menu.layout.toggle_rows[0]) ||
           g_menu.layout.toggle_rows[1].top!=label_row.top)
            throw std::runtime_error("hidden feature left a clickable row or blank space in the main menu");
        save(L"menu-main-customized.bmp");
        menu_open_config(ipc::kFeatureDamageNumbers);
        save(L"menu-statistics-top.bmp");
        for (const auto bit : {ipc::kFeatureDamageNumbers,ipc::kFeatureSpawnList,ipc::kFeatureWiki,ipc::kFeatureEnemyPrts}) {
            menu_open_config(bit);
            click_rect(row_switch(click_through_row()),HitTarget::kClickThrough);
            if(!feature_appearance(config,bit).click_through) throw std::runtime_error("menu cannot enable this panel's click-through");
            click_rect(row_switch(click_through_row()),HitTarget::kClickThrough);
            if(feature_appearance(config,bit).click_through) throw std::runtime_error("menu cannot restore panel interaction");
        }
        menu_open_config(ipc::kFeatureDamageNumbers);
        click_rect(row_switch(damage_row_rect(g_menu.layout,ipc::kStatCount+3)),HitTarget::kDamageCompact);
        if(config.damage_compact) throw std::runtime_error("menu cannot disable compact table spacing");
        click_rect(row_switch(damage_row_rect(g_menu.layout,ipc::kStatCount+3)),HitTarget::kDamageCompact);
        if(!config.damage_compact) throw std::runtime_error("menu cannot restore compact table spacing");
        click_rect(row_switch(damage_row_rect(g_menu.layout,ipc::kStatCount+4)),HitTarget::kDamageDevices);
        if(!config.damage_show_devices || !config.overlay.operator_show_devices) throw std::runtime_error("statistics device switch did not enable independently");
        click_rect(row_switch(damage_row_rect(g_menu.layout,ipc::kStatCount+4)),HitTarget::kDamageDevices);
        if(config.damage_show_devices || !config.overlay.operator_show_devices) throw std::runtime_error("statistics device switch changed operator display");
        if (g_menu.viewport.maximum_scroll() <= 0) throw std::runtime_error("long statistics configuration cannot scroll");
        const auto fixed_footer = g_menu.layout.back_button;
        const auto order=statistics::parse_order(config.damage_order);
        const auto old_metrics=config.damage_metric_mask;
        click_rect(damage_row_rect(g_menu.layout,8),static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+8));
        if(config.damage_metric_mask!=(old_metrics^(1u<<order[8])))
            throw std::runtime_error("scrolled statistics toggled a different metric");
        g_menu.viewport.move(g_menu.viewport.maximum_scroll());
        save(L"menu-statistics-bottom.bmp");
        const POINT footer{(fixed_footer.left+fixed_footer.right)/2, (fixed_footer.top+fixed_footer.bottom)/2};
        if (hit_test(footer) != HitTarget::kBack) throw std::runtime_error("scrolling changed fixed footer hit testing");
        config.ui_scale = 150;
        g_menu.work = RECT{0, 0, 1000, 600};
        menu_sync_config(config);
        open_page(0);
        save(L"menu-short-screen.bmp");
        if (g_menu.surface.height > 600 || g_menu.surface.width > 1000 || g_menu.viewport.maximum_scroll() <= 0)
            throw std::runtime_error("scaled short-screen menu exceeds work area or cannot scroll");
        if(!panels::initialize(GetModuleHandleW(nullptr),{})) throw std::runtime_error("panel review creation failed");
        auto* panel=panels::g_panels[0];
        int settings_clicks=0,row_clicks=0;
        panels::g_callbacks.on_settings=[&](const wchar_t*) { ++settings_clicks; };
        panels::g_callbacks.on_row_clicked=[&](const wchar_t*,int) { ++row_clicks; };
        panel->title=L"数值统计";
        panels::Row header;header.header=true;header.cells={L"干员",L"总伤害",L"治疗"};
        panels::Row total=header;total.cells={L"全队合计",L"123456",L"789"};
        panels::Row actor;actor.cells={L"示例干员",L"123456",L"789"};
        actor.tip=L"此内容应当完全不显示为悬浮提示";
        panel->rows={header,total,actor};
        panels::render(*panel);
        const auto body=panel->row_rects[2];const POINT body_point{(body.left+body.right)/2,(body.top+body.bottom)/2};
        SendMessageW(panel->hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(body_point.x,body_point.y));
        SendMessageW(panel->hwnd,WM_LBUTTONUP,0,MAKELPARAM(body_point.x,body_point.y));
        SendMessageW(panel->hwnd,WM_RBUTTONUP,0,MAKELPARAM(body_point.x,body_point.y));
        if(row_clicks || settings_clicks) throw std::runtime_error("statistics body still opens settings");
        for(auto* item:panels::g_panels) {
            panels::render(*item);
            const POINT point{(item->settings_button.left+item->settings_button.right)/2,(item->settings_button.top+item->settings_button.bottom)/2};
            const int before=settings_clicks;
            SendMessageW(item->hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(point.x,point.y));
            if(settings_clicks!=before) throw std::runtime_error("panel settings opens before a completed click");
            SendMessageW(item->hwnd,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
            if(settings_clicks!=before+1) throw std::runtime_error("panel settings icon missed callback");
            SendMessageW(item->hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(point.x,point.y));
            SendMessageW(item->hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x+panels::S(*item,20),point.y));
            SendMessageW(item->hwnd,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
            if(settings_clicks!=before+1) throw std::runtime_error("dragging panel settings still activated it");
        }
        FeatureAppearance custom;custom.opacity=60;custom.font_size=32;custom.text_color=0x12ABCD;
        panels::set_appearance(panels::kDamagePanelId,custom);
        panels::render(*panel);
        if(panels::row_height(*panel)<panel->measured_row_height+panels::S(*panel,4) ||
           panels::title_height(*panel)<panel->measured_title_height+panels::S(*panel,10))
            throw std::runtime_error("custom panel font is taller than its row or title");
        if(panels::panel_alpha(*panel)!=153 || panels::panel_text_color(*panel,RGB(255,255,255))!=RGB(0x12,0xAB,0xCD))
            throw std::runtime_error("panel opacity or RGB conversion is incorrect");
        if(panel->horizontal_track.right<=panel->horizontal_track.left)
            throw std::runtime_error("large font made the numeric scrollbar unreachable in a narrow panel");
        panel->width_override_px=1000;
        panels::render(*panel);
        save_review_bitmap(panel->surface,destination/L"panel-statistics-large-font.bmp");
        panels::set_appearance(panels::kDamagePanelId,{});
        panels::render(*panel);
        if(panels::panel_alpha(*panel)!=255 || panels::row_height(*panel)!=panels::S(*panel,24))
            throw std::runtime_error("panel default appearance did not restore");
        panels::set_statistics_compact(false);
        if(panels::row_height(*panel)!=panels::S(*panel,30)) throw std::runtime_error("loose row spacing is not available");
        panels::set_statistics_compact(true);
        const int manual_width=panel->width_override_px;
        FeatureAppearance transparent;transparent.opacity=55;transparent.text_color=0x556677;
        transparent.click_through=true;
        panels::set_appearance(panels::kDamagePanelId,transparent);
        if(!(GetWindowLongPtrW(panel->hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT) || panel->width_override_px!=manual_width)
            throw std::runtime_error("click-through did not use the layered window style or appearance reset manual width");
        panels::set_appearance(panels::kDamagePanelId,{});
        if(GetWindowLongPtrW(panel->hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT)
            throw std::runtime_error("click-through cannot be switched back off");
        // Replay queued screen coordinates without moving the user's cursor or capturing native input.
        const int previous_x=panel->x,previous_y=panel->y;
        panel->x=-30000;panel->y=-30000;
        SetWindowPos(panel->hwnd,nullptr,panel->x,panel->y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        POINT shared_title{panel->title_bar.left+40,(panel->title_bar.top+panel->title_bar.bottom)/2};
        ClientToScreen(panel->hwnd,&shared_title);
        panels::send_shared_mouse(*panel,{WM_LBUTTONDOWN,shared_title,MK_LBUTTON});
        if(!panel->dragging || GetCapture()==panel->hwnd) throw std::runtime_error("shared title drag did not begin without stealing capture");
        POINT shared_moved{shared_title.x+72,shared_title.y+48};
        panels::send_shared_mouse(*panel,{WM_MOUSEMOVE,shared_moved,MK_LBUTTON});
        if(panel->x!=-30000+72 || panel->y!=-30000+48) throw std::runtime_error("shared drag did not use queued event coordinates");
        panels::send_shared_mouse(*panel,{WM_LBUTTONUP,shared_moved,0});
        if(panel->dragging || GetCapture()==panel->hwnd) throw std::runtime_error("shared drag remained active after release");
        POINT shared_edge{panel->width_grip.right-1,(panel->title_bar.top+panel->title_bar.bottom)/2};
        ClientToScreen(panel->hwnd,&shared_edge);
        const int shared_width=panel->width_override_px;
        panels::send_shared_mouse(*panel,{WM_LBUTTONDOWN,shared_edge,MK_LBUTTON});
        shared_edge.x-=45;
        panels::send_shared_mouse(*panel,{WM_MOUSEMOVE,shared_edge,MK_LBUTTON});
        panels::send_shared_mouse(*panel,{WM_LBUTTONUP,shared_edge,0});
        if(panel->width_override_px!=std::clamp(shared_width-45,panels::S(*panel,260),900) || panel->resizing_width || GetCapture()==panel->hwnd)
            throw std::runtime_error("shared resize lost its copied input or captured native mouse");
        panel->x=previous_x;panel->y=previous_y;
        SetWindowPos(panel->hwnd,nullptr,panel->x,panel->y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        panel->width_override_px=panels::S(*panel,260);
        panel->ui_scale=100;
        panels::destroy_fonts(*panel);
        panel->rows[0].cells={L"干员",L"物理",L"有效回费"};
        panel->rows[1].cells={L"全队合计",L"58.9k",L"36"};
        panel->rows[2].cells={L"圣聆初雪",L"0",L"0"};
        panels::Row second;second.cells={L"凛御银灰",L"58.9k",L"36"};panel->rows.push_back(second);
        panels::render(*panel);
        if(panel->max_horizontal!=0) throw std::runtime_error("compact table still scrolls despite sufficient text space");
        save_review_bitmap(panel->surface,destination/L"panel-statistics-compact.bmp");
        panel->rows[0].cells={L"干员",L"物理",L"有效回费",L"有效治疗",L"元素积累",L"总伤害",L"DPS"};
        panels::render(*panel);
        if(panel->max_horizontal<=0) throw std::runtime_error("genuinely overflowing columns cannot scroll");
        panels::fit_statistics_width(panel->rows);
        panels::render(*panel);
        save_review_bitmap(panel->surface,destination/L"panel-statistics-fitted.bmp");
        if(panels::g_panels[2]->hwnd==panels::g_panels[3]->hwnd ||
            std::wcscmp(panels::g_panels[3]->id,panels::kPrtsPanelId)!=0)
            throw std::runtime_error("PRTS did not create an independent panel");
        int width_commits=0;
        panels::g_callbacks.on_resized=[&](const wchar_t*,int width,int) { if(width>0) ++width_commits; };
        for(auto* collapsed:panels::g_panels) {
            collapsed->collapsed=true;collapsed->closeable=true;collapsed->width_override_px=panels::S(*collapsed,320);
            panels::render(*collapsed);
            const POINT edge{collapsed->width_grip.right-1,(collapsed->title_bar.top+collapsed->title_bar.bottom)/2};
            if(PtInRect(&collapsed->close_button,edge) || PtInRect(&collapsed->toggle,edge))
                throw std::runtime_error("collapsed width edge overlaps a title button");
            const int before=collapsed->width_override_px;
            SendMessageW(collapsed->hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(edge.x,edge.y));
            if(!collapsed->resizing_width || collapsed->dragging || !collapsed->collapsed)
                throw std::runtime_error("collapsed width drag instead moved, closed or expanded the panel");
            collapsed->width_resize_cursor_start.x-=80; // simulate delta without moving the real cursor
            SendMessageW(collapsed->hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge.x+80,edge.y));
            SendMessageW(collapsed->hwnd,WM_LBUTTONUP,0,0);
            if(collapsed->width_override_px!=before+80 || !collapsed->collapsed || collapsed->resizing_width)
                throw std::runtime_error("collapsed width drag failed to persist its new size");
            if(collapsed==panel) save_review_bitmap(collapsed->surface,destination/L"panel-collapsed-resized.bmp");
            collapsed->collapsed=false;panels::render(*collapsed);
            if(collapsed->content.right-collapsed->content.left!=before+80)
                throw std::runtime_error("expanding discarded the width set while collapsed");
        }
        if(width_commits!=4) throw std::runtime_error("not all collapsed panels saved their resized widths");
        auto* wiki=panels::g_panels[2];auto* prts=panels::g_panels[3];
        wiki->visible=true;panels::hide(*prts);wiki->collapsed=false;
        panels::g_dock_groups.push_back({{wiki,prts}});
        panels::render(*wiki);
        const POINT dock_edge{wiki->width_grip.right-1,(wiki->title_bar.top+wiki->title_bar.bottom)/2};
        SendMessageW(wiki->hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(dock_edge.x,dock_edge.y));
        wiki->width_resize_cursor_start.x-=60;
        SendMessageW(wiki->hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(dock_edge.x+60,dock_edge.y));
        if(prts->visible || IsWindowVisible(prts->hwnd)) throw std::runtime_error("resizing a dock group showed its disabled PRTS member");
        SendMessageW(wiki->hwnd,WM_LBUTTONUP,0,0);
        if(wiki->width_override_px!=prts->width_override_px || width_commits!=6)
            throw std::runtime_error("hidden dock member width did not save without showing it");
        panels::g_dock_groups.clear();
        wiki->visible=false;prts->visible=false;
        wiki->width_override_px=panels::S(*wiki,180);
        panels::Row long_text;long_text.left=L"名称";long_text.right=L"这是被截断时必须完整显示的敌人名称和完整详情";
        wiki->rows={long_text};wiki->title=L"完整显示测试";panels::render(*wiki);
        const auto& hints=wiki->hints.items();
        auto found=std::find_if(hints.begin(),hints.end(),[&](const auto& item){return item.text==long_text.right;});
        if(found==hints.end() || wiki->hints.text_at({(found->rect.left+found->rect.right)/2,(found->rect.top+found->rect.bottom)/2})!=long_text.right)
            throw std::runtime_error("panel value overflow did not retain its full Unicode text");
        save_review_bitmap(wiki->surface,destination/L"panel-overflow-content.bmp");
        ShowWindow(wiki->hwnd,SW_SHOWNOACTIVATE); // fixture stays offscreen; no focus or cursor changes
        const POINT hint_point{(found->rect.left+found->rect.right)/2,(found->rect.top+found->rect.bottom)/2};
        wiki->hints.hover(hint_point);
        struct TooltipProbe { HWND owner,window; } probe{wiki->hwnd,nullptr};
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM parameter) -> BOOL {
            auto& probe=*reinterpret_cast<TooltipProbe*>(parameter);
            wchar_t name[64]{};GetClassNameW(window,name,64);
            if(GetWindow(window,GW_OWNER)==probe.owner && std::wcscmp(name,TOOLTIPS_CLASSW)==0) probe.window=window;
            return TRUE;
        },reinterpret_cast<LPARAM>(&probe));
        wchar_t full_text[1024]{};TOOLINFOW tool{};tool.cbSize=TTTOOLINFOW_V2_SIZE;tool.hwnd=wiki->hwnd;tool.uId=1;tool.lpszText=full_text;
        if(probe.window) SendMessageW(probe.window,TTM_GETTEXTW,1024,reinterpret_cast<LPARAM>(&tool));
        const bool native_hint=probe.window && IsWindowVisible(probe.window) && long_text.right==full_text;
        wiki->hints.hide();ShowWindow(wiki->hwnd,SW_HIDE);
        if(!native_hint) throw std::runtime_error("native hover tooltip did not show complete Unicode contents");
        // The shared detector also covers complete text, wrapping, and external cell clipping.
        LayeredBitmap scratch;layered_bitmap_create(scratch,300,180);OverflowHints overflow;
        const auto font=create_ui_font(13,FW_NORMAL,96,100);
        overflow.begin(wiki->hwnd,scratch.dc);
        draw_text(scratch.dc,L"完整短文本",RECT{0,0,280,30},font,RGB(255,255,255),DT_SINGLELINE|DT_END_ELLIPSIS);
        draw_text(scratch.dc,L"这是一段需要多行但只有一行高度的完整说明文字",RECT{0,40,80,65},font,RGB(255,255,255),DT_WORDBREAK|DT_END_ELLIPSIS);
        const int saved=SaveDC(scratch.dc);IntersectClipRect(scratch.dc,120,100,150,130);
        draw_text(scratch.dc,L"被外部单元格边界截断的文字",RECT{120,100,290,130},font,RGB(255,255,255),DT_SINGLELINE|DT_RIGHT);
        RestoreDC(scratch.dc,saved);overflow.end();
        if(!overflow.text_at({30,15}).empty() || overflow.text_at({30,50}).empty() || overflow.text_at({130,110}).empty() ||
           !overflow.text_at({200,110}).empty()) throw std::runtime_error("global overflow detection missed wrapping/external clip or hinted complete text");
        overflow.destroy();DeleteObject(font);layered_bitmap_destroy(scratch);
        menu_open_settings();config.prts_cache_path=L"D:\\一个非常长的缓存目录名称\\用于验证全部菜单共享截断检测\\包含中文和空格的完整路径\\PRTS 具体资料和头像缓存";
        menu_sync_config(config);
        const auto cache_area=cache_path_button(0);
        g_menu.viewport.scroll=std::clamp<int>(cache_area.top-g_menu.viewport.top-S(100),0,g_menu.viewport.maximum_scroll());render();
        const auto& menu_hints=g_menu.hints.items();
        if(std::none_of(menu_hints.begin(),menu_hints.end(),[&](const auto& item){return item.text==config.prts_cache_path;}))
            throw std::runtime_error("scrolled menu path did not expose its complete text on hover");
        prts->ui_scale=100;prts->width_override_px=panels::S(*prts,300);
        prts->height_override_px=panels::S(*prts,430);prts->scroll_px=0;
        prts->title=L"敌方 PRTS";panels::destroy_fonts(*prts);
        panels::Row reference_icon;reference_icon.icon_only=true;reference_icon.left=L"示例敌人";reference_icon.selected=true;
        panels::Row reference_header;reference_header.header=true;reference_header.cells={L"级别 0 · 参考数值"};
        panels::Row reference_table;reference_table.table=true;
        reference_table.cells={L"最大生命值",L"18300",L"攻击力",L"1200",L"防御力",L"850",L"法术抗性",L"50",L"移动速度",L"0.75",L"攻击间隔",L"3.5"};
        panels::Row reference_paragraph;reference_paragraph.wrap=true;
        reference_paragraph.left=L"特性：这是一段包含中英文 Mixed text 和完整技能描述的资料，用来检查根据窗口宽度自动换行，而不是固定二十六个字截断。\n暂停时可以查看具体效果；拖动边缘后，文字和属性表应当立即重新排布。";
        prts->rows={reference_icon,reference_header,reference_table,reference_paragraph};
        panels::set_avatars(panels::kPrtsPanelId,{nullptr},28);
        panels::render(*prts);
        const int narrow_height=prts->row_heights[3],narrow_columns=prts->table_columns[2];
        if(narrow_columns>=3 || narrow_height<=panels::row_height(*prts)) throw std::runtime_error("narrow PRTS did not wrap paragraphs or reduce grid columns");
        save_review_bitmap(prts->surface,destination/L"panel-prts-narrow.bmp");
        prts->width_override_px=panels::S(*prts,740);panels::render(*prts);
        if(prts->table_columns[2]!=3 || prts->row_heights[3]>=narrow_height) throw std::runtime_error("wide PRTS failed to reflow paragraphs and three grid columns");
        save_review_bitmap(prts->surface,destination/L"panel-prts-wide.bmp");
        prts->width_override_px=panels::S(*prts,300);prts->height_override_px=panels::S(*prts,180);
        prts->rows[3].left+=prts->rows[3].left+prts->rows[3].left;
        panels::render(*prts);prts->scroll_px=prts->max_scroll;panels::render(*prts);
        if(prts->max_scroll<=0 || IsRectEmpty(&prts->row_rects[3])) throw std::runtime_error("paragraph taller than the viewport became unreachable when scrolling");
        panels::set_appearance(panels::kPrtsPanelId,custom);
        panels::render(*prts);
        if(prts->row_heights[3]<=narrow_height) throw std::runtime_error("PRTS reflow ignored custom font size");
        panels::set_appearance(panels::kPrtsPanelId,{});
        prts->rows.assign(130,reference_icon);
        prts->rows.push_back(reference_paragraph);
        panels::set_avatars(panels::kPrtsPanelId,std::vector<HBITMAP>(130,nullptr),28);
        panels::render(*prts);
        if(prts->max_icon_scroll<=0 || !IsRectEmpty(&prts->row_rects[129])) throw std::runtime_error("long enemy roster was not clipped and scrollable");
        POINT icon_wheel{(prts->icon_viewport.left+prts->icon_viewport.right)/2,prts->icon_viewport.top+10};
        ClientToScreen(prts->hwnd,&icon_wheel);
        const int detail_scroll=prts->scroll_px;
        for(int i=0;i<4;++i) SendMessageW(prts->hwnd,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA*20)),MAKELPARAM(icon_wheel.x,icon_wheel.y));
        if(prts->icon_scroll_px!=prts->max_icon_scroll || IsRectEmpty(&prts->row_rects[129]) || prts->scroll_px!=detail_scroll)
            throw std::runtime_error("full enemy roster bottom is unreachable or scrolling it moved the selected enemy detail");
        int selected_icon=-1;
        panels::g_callbacks.on_row_clicked=[&](const wchar_t*,int index) { selected_icon=index; };
        const auto last_icon=prts->row_rects[129];
        SendMessageW(prts->hwnd,WM_LBUTTONDOWN,0,MAKELPARAM((last_icon.left+last_icon.right)/2,(last_icon.top+last_icon.bottom)/2));
        if(selected_icon!=129) throw std::runtime_error("scrolled enemy avatar selected the wrong roster entry");
        save_review_bitmap(prts->surface,destination/L"panel-prts-full-roster-bottom.bmp");
        panels::shutdown();
        menu_destroy();
        CoUninitialize();
        std::puts("PASS real UI: bottom-edge menu height, default order, four panel settings, inert statistics body, hidden dock resize, full native Unicode hover, clipping/scroll mapping and prior features");
    } catch (const std::exception& error) {
        panels::shutdown(); menu_destroy(); CoUninitialize();
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
    return 0;
}
