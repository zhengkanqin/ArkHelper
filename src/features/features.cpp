#include "features.h"
#include "../data/enemy_metrics.h"
#include <limits>
#include "range_contour.h"
#include "../data/battle_statistics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace arkhelp::features {
namespace {

constexpr gfx::Color kTimerColor = {1.0f, 0.30f, 0.34f, 1.0f};
constexpr gfx::Color kFallbackColor = {1.0f, 0.82f, 0.84f, 1.0f};
constexpr gfx::Color kCardFill = {0.04f, 0.06f, 0.09f, 0.72f};
constexpr gfx::Color kCardBorder = {0.85f, 0.90f, 1.0f, 0.22f};
constexpr gfx::Color kTitleColor = {0.96f, 0.97f, 1.0f, 1.0f};
constexpr gfx::Color kTextColor = {0.80f, 0.84f, 0.90f, 1.0f};
constexpr gfx::Color kMutedColor = {0.62f, 0.66f, 0.72f, 0.95f};

constexpr float kCardWidth = 180.0f;
constexpr float kCardHeight = 138.0f;
constexpr float kCardRightMargin = 220.0f;
constexpr float kCardGateOffsetY = 26.0f;
constexpr gfx::Color kHpGood{0.35f, 0.85f, 0.45f, 0.95f};
constexpr gfx::Color kHpWarn{0.95f, 0.80f, 0.30f, 0.95f};
constexpr gfx::Color kHpBad{0.95f, 0.35f, 0.35f, 0.95f};
constexpr float kCardTop = 120.0f;
constexpr float kTimerSize = 22.0f;
constexpr float kTimerMaxWidth = 96.0f;
constexpr float kTimerOffsetX = 14.0f;
constexpr float kTimerOffsetY = 10.0f;
constexpr float kFallbackSize = 16.0f;
constexpr float kDamageSize = 22.0f;
constexpr float kDamageOffsetX = -24.0f;
constexpr float kDamageOffsetY = -40.0f;
constexpr float kDamageFallbackSize = 16.0f;
constexpr float kFallbackLine = 20.0f;

std::wstring utf8_to_wide_field(const char* text, std::size_t capacity) {
    if (text == nullptr || capacity == 0) {
        return {};
    }
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    if (length == 0) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), result.data(), count);
    return result;
}

std::wstring truncate(std::wstring text, std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    text.resize(max_chars);
    text.push_back(L'…');
    return text;
}

int clamped_spawn_count(const ipc::BattleSnapshot& snapshot) {
    return std::min<int>(static_cast<int>(snapshot.spawn_count), ipc::kMaxSpawns);
}

int clamped_enemy_count(const ipc::BattleSnapshot& snapshot) {
    return std::min<int>(static_cast<int>(snapshot.enemy_count), ipc::kMaxEnemies);
}

int clamped_damage_count(const ipc::BattleSnapshot& snapshot) {
    return std::min<int>(static_cast<int>(snapshot.damage_count), ipc::kMaxDamageRows);
}

std::wstring spawn_label(const ipc::BattleSnapshot& snapshot, int index) {
    const ipc::SpawnEntry& spawn = snapshot.spawns[index];
    std::wstring name = utf8_to_wide_field(spawn.enemy_name, ipc::kNameCapacity);
    if (name.empty()) {
        name = utf8_to_wide_field(spawn.enemy_key, ipc::kIdCapacity);
    }
    if (name.empty()) {
        name = L"未知敌人";
    }
    return name;
}

std::wstring format_scaled(std::int64_t tenths, const wchar_t* suffix) {
    const std::int64_t whole = tenths / 10;
    const std::int64_t fraction = tenths % 10;
    std::wstring text = std::to_wstring(whole);
    if (fraction != 0) {
        text.push_back(L'.');
        text.push_back(static_cast<wchar_t>(L'0' + static_cast<wchar_t>(fraction)));
    }
    text += suffix;
    return text;
}

bool same_key(const char* left, const char* right) {
    return std::strncmp(left, right, ipc::kIdCapacity) == 0;
}

float integer_seconds(float remaining) {
    const float rounded = std::floor(remaining + 0.5f);
    if (rounded < 0.0f) {
        return 0.0f;
    }
    return rounded;
}

}  // namespace

gfx::Color damage_color(std::uint32_t damage_type) {
    switch (damage_type) {
        case ipc::kDamagePhysical:
            return gfx::Color{1.0f, 0.42f, 0.42f, 1.0f};  // #FF6B6B
        case ipc::kDamageArts:
            return gfx::Color{0.31f, 0.66f, 1.0f, 1.0f};  // #4FA8FF
        case ipc::kDamageTrue:
            return gfx::Color{1.0f, 1.0f, 1.0f, 1.0f};  // #FFFFFF
        case ipc::kDamageHeal:
            return gfx::Color{0.42f, 0.88f, 0.54f, 1.0f};  // #6BE08A
        case ipc::kDamageElemental:
            return gfx::Color{0.75f, 0.55f, 1.0f, 1.0f};  // #C08BFF
        default:
            return gfx::Color{0.92f, 0.92f, 0.92f, 1.0f};
    }
}

std::wstring format_damage(std::int64_t value) {
    const bool negative = value < 0;
    const std::int64_t magnitude = negative ? -value : value;
    std::wstring text;
    if (magnitude < 1000) {
        text = std::to_wstring(magnitude);
    } else if (magnitude < 999500) {
        const std::int64_t tenths = (magnitude * 10 + 500) / 1000;
        text = format_scaled(tenths, L"k");
    } else {
        const std::int64_t tenths = (magnitude * 10 + 500000) / 1000000;
        text = format_scaled(tenths, L"M");
    }
    if (negative) {
        text.insert(text.begin(), L'-');
    }
    return text;
}


namespace {
// Rough text width estimate: CJK glyphs are square, ASCII is a bit narrower.
float estimate_text_width(const std::wstring& text, float size) {
    float width = 0.0f;
    for (const wchar_t ch : text) {
        width += ch < 0x80 ? size * 0.55f : size;
    }
    return width;
}
} // namespace

std::wstring format_clock(float seconds) {
    const int total = static_cast<int>(seconds + 0.5f);
    const int minutes = total / 60;
    const int rest = total % 60;
    wchar_t buffer[16] = {};
    std::swprintf(buffer, 16, L"%02d:%02d", minutes, rest);
    return buffer;
}

void draw_spawn_timers(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                       bool only_paused, bool paused, float font_size) {
    if (only_paused && !paused) {
        return;
    }
    const int count = clamped_spawn_count(snapshot);
    if (count <= 0) {
        return;
    }
    const float size = (font_size > 0 ? font_size : kTimerSize) * scale;
    const float max_width = kTimerMaxWidth * scale;
    // 智能分波：同一红门的出怪按时间排序后贪心聚类，同一波内相邻事件间隔不超过 5 秒
    // （5 秒是参考值而非硬边界，避免 4.9/5.1 秒的怪被拆成两波）。
    // 每波合并成「名字×数量」，右侧给出这一波的首只怪出现时间；不做 20 秒截断。
    struct BatchItem
    {
        std::wstring name;
        int amount = 0;
    };
    struct GateBatch
    {
        ipc::Vec2 point;
        float remaining = 0.0f;
        bool pending = false;
        std::vector<BatchItem> items;
    };
    struct Candidate
    {
        float remaining = 0.0f;
        int index = 0;
    };
    std::vector<GateBatch> gates;
    std::vector<std::vector<Candidate>> gate_candidates;
    std::vector<std::pair<std::wstring, std::wstring>> fallback;
    for (int index = 0; index < count; ++index) {
        const ipc::SpawnEntry& spawn = snapshot.spawns[index];
        const float remaining = spawn.remaining_seconds;
        const bool pending = (spawn.flags & ipc::kSpawnPending) != 0;
        if (!pending && !(remaining >= 0.0f)) {
            continue;
        }
        if ((spawn.flags & ipc::kSpawnScreenValid) == 0) {
            fallback.emplace_back(pending ? L"待触发" : format_clock(remaining), spawn_label(snapshot, index));
            continue;
        }
        std::size_t gate_index = gates.size();
        for (std::size_t probe = 0; probe < gates.size(); ++probe) {
            if (gates[probe].point.x == spawn.screen.x &&
                gates[probe].point.y == spawn.screen.y) {
                gate_index = probe;
                break;
            }
        }
        if (gate_index == gates.size()) {
            gates.push_back(GateBatch{spawn.screen, 0.0f, pending, {}});
            gate_candidates.emplace_back();
        }
        gate_candidates[gate_index].push_back(Candidate{remaining, index});
    }
    constexpr float kWaveWindow = 5.0f;
    for (std::size_t gate_index = 0; gate_index < gates.size(); ++gate_index) {
        auto& candidates = gate_candidates[gate_index];
        if (candidates.empty()) {
            continue;
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& left, const Candidate& right) {
                      return left.remaining < right.remaining;
                  });
        GateBatch& gate = gates[gate_index];
        gate.remaining = candidates.front().remaining;
        float wave_start = candidates.front().remaining;
        for (const Candidate& candidate : candidates) {
            if (candidate.remaining - wave_start > kWaveWindow) {
                break;  // 只展示最近的一波，后续波次等这一波过去后再出现。
            }
            const std::wstring name = spawn_label(snapshot, candidate.index);
            const int amount = static_cast<int>(
                snapshot.spawns[candidate.index].count > 0 ? snapshot.spawns[candidate.index].count : 1);
            bool merged = false;
            for (BatchItem& item : gate.items) {
                if (item.name == name) {
                    item.amount += amount;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                gate.items.push_back(BatchItem{name, amount});
            }
        }
    }
    // 每一波按「列」排布：每行一个敌人条目（名字×数量），最后一行是出现时间。
    for (const GateBatch& gate : gates) {
        if (gate.items.empty()) {
            continue;
        }
        std::vector<std::wstring> lines;
        lines.reserve(gate.items.size() + 1);
        for (const BatchItem& item : gate.items) {
            std::wstring line = item.name;
            line += L"×";
            line += std::to_wstring(item.amount);
            lines.push_back(line);
        }
        lines.push_back(gate.pending ? L"待触发" : format_clock(gate.remaining));
        float text_size = size;
        for (const std::wstring& line : lines) {
            while (font_size == 0 && text_size > 11.0f * scale && estimate_text_width(line, text_size) > max_width) {
                text_size -= 1.0f * scale;
            }
        }
        const float line_height = text_size * 1.15f;
        const float total_height = line_height * static_cast<float>(lines.size());
        float top = static_cast<float>(gate.point.y) - total_height * 0.5f - kTimerOffsetY * scale;
        for (const std::wstring& line : lines) {
            const float left = static_cast<float>(gate.point.x) + kTimerOffsetX * scale;
            surface.draw_text(line, left, top, text_size, kTimerColor, gfx::TextAlign::kCenter,
                              true);
            top += line_height;
        }
    }
    if (fallback.empty()) {
        return;
    }
    // Projection was unavailable for these entries: keep a short list so the info is not lost.
    const float right = static_cast<float>(surface.width()) - kCardRightMargin * scale;
    float y = 74.0f * scale;
    const float fallback_size = (font_size > 0 ? font_size : kFallbackSize) * scale;
    const float fallback_line = std::max(kFallbackLine * scale, fallback_size * 1.25f);
    surface.draw_text(L"出怪倒计时", right, y, fallback_size, kFallbackColor,
                      gfx::TextAlign::kLeading, false);
    y += fallback_line;
    for (const auto& item : fallback) {
        if (y > static_cast<float>(surface.height()) - fallback_line) {
            break;
        }
        std::wstring line = item.first;
        line += L"  ";
        line += item.second;
        surface.draw_text(line, right, y, fallback_size, kFallbackColor,
                          gfx::TextAlign::kLeading, false);
        y += fallback_line;
    }
}

void draw_spawn_card(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot,
                     std::uint32_t card_index, float scale) {
    const int count = clamped_spawn_count(snapshot);
    if (count <= 0) {
        return;
    }
    const ipc::SpawnEntry& spawn =
        snapshot.spawns[card_index % static_cast<std::uint32_t>(count)];
    const float width = kCardWidth * scale;
    const float height = kCardHeight * scale;
    // When the gate position is known, attach the card to the gate (below the countdown label);
    // otherwise keep the legacy top-right slot.
    float left = static_cast<float>(surface.width()) - kCardRightMargin * scale - width;
    float top = kCardTop * scale;
    if ((spawn.flags & ipc::kSpawnScreenValid) != 0) {
        left = static_cast<float>(spawn.screen.x) - width * 0.5f;
        top = static_cast<float>(spawn.screen.y) + kCardGateOffsetY * scale;
        const float margin = 8.0f * scale;
        if (left < margin) {
            left = margin;
        }
        if (top + height > static_cast<float>(surface.height()) - margin) {
            top = static_cast<float>(spawn.screen.y) - height - kCardGateOffsetY * scale;
        }
        if (top < margin) {
            top = margin;
        }
    }
    surface.fill_rounded_rect(left, top, left + width, top + height, 8.0f * scale, kCardFill);
    surface.stroke_rounded_rect(left, top, left + width, top + height, 8.0f * scale, kCardBorder);

    const float padding = 10.0f * scale;
    float y = top + 8.0f * scale;
    surface.draw_text(truncate(spawn_label(snapshot, static_cast<int>(card_index) % count), 10),
                      left + padding, y, 16.0f * scale, kTitleColor, gfx::TextAlign::kLeading,
                      false);
    y += 22.0f * scale;

    std::wstring line = L"×" + std::to_wstring(spawn.count);
    line += L"   路线 ";
    line += std::to_wstring(spawn.route_index + 1);
    if ((spawn.flags & ipc::kSpawnRandom) != 0) {
        line += L"（随机）";
    }
    surface.draw_text(line, left + padding, y, 12.5f * scale, kTextColor,
                      gfx::TextAlign::kLeading, false);
    y += 18.0f * scale;

    if (spawn.flags & ipc::kSpawnPending) {
        surface.draw_text(L"待触发", left + padding, y, 12.5f * scale, kTextColor,
                          gfx::TextAlign::kLeading, false);
        y += 18.0f * scale;
    } else if (spawn.remaining_seconds >= 0.0f) {
        std::wstring remaining = L"剩余 ";
        remaining += std::to_wstring(static_cast<int>(integer_seconds(spawn.remaining_seconds)));
        remaining += L" 秒";
        surface.draw_text(remaining, left + padding, y, 12.5f * scale, kTextColor,
                          gfx::TextAlign::kLeading, false);
        y += 18.0f * scale;
    }

    const ipc::EnemyEntry* reference = nullptr;
    const int enemies = clamped_enemy_count(snapshot);
    for (int index = 0; index < enemies; ++index) {
        const ipc::EnemyEntry& enemy = snapshot.enemies[index];
        if ((enemy.flags & ipc::kEnemyValid) == 0 ||
            (enemy.flags & ipc::kEnemyReference) == 0 || (enemy.flags & ipc::kEnemyRuntime) != 0) {
            continue;
        }
        if (!same_key(enemy.enemy_key, spawn.enemy_key)) {
            continue;
        }
        reference = &enemy;
        break;
    }
    // A live enemy with the same template can carry different stage/mode buffs. Its current HP
    // and attributes must never be presented as the stats of the next planned spawn.
    const auto stat = [&](std::uint32_t bit, std::int64_t value) {
        return reference && (reference->stats_mask & bit) ? format_damage(value) : std::wstring(L"—");
    };
    const std::wstring hp = stat(ipc::kEnemyMaxHp, reference ? reference->max_hp : 0);
    const std::wstring atk = stat(ipc::kEnemyAtk, reference ? reference->atk : 0);
    const std::wstring def = stat(ipc::kEnemyDef, reference ? reference->def : 0);
    surface.draw_text(L"HP " + hp, left + padding, y, 12.5f * scale,
                      kTextColor, gfx::TextAlign::kLeading, false);
    y += 18.0f * scale;
    surface.draw_text(L"ATK " + atk + L"   DEF " + def, left + padding, y, 12.5f * scale,
                      kTextColor, gfx::TextAlign::kLeading, false);
    y += 20.0f * scale;
    const std::wstring source = reference ? L"关卡定义参考 · 非实战数值"
                                          : L"出场后可见，以实例实测为准";
    surface.draw_text(source, left + padding, y, 10.0f * scale,
                      kMutedColor, gfx::TextAlign::kLeading, false);
}


void draw_hp_bars(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                  bool only_paused, bool paused, float font_size,
                  const OverlayOptions& options,const PointerState& pointer) {
    if (only_paused && !paused) {
        return;
    }
    const int count = static_cast<int>(
        snapshot.unit_count < static_cast<std::uint32_t>(ipc::kMaxUnits) ? snapshot.unit_count
                                                                         : ipc::kMaxUnits);
    for (int index = 0; index < count; ++index) {
        const ipc::UnitEntry& unit = snapshot.units[index];
        if ((unit.flags & (ipc::kUnitValid | ipc::kUnitHpValid)) !=
            (ipc::kUnitValid | ipc::kUnitHpValid) || !std::isfinite(unit.hp) || unit.hp < 0.0f) {
            continue;
        }
        // 游戏本身已有血条，这里只补「可见的数字」：不再画绿色血条，避免重复。
        if(options.hp_only_hover && !pointer_near(pointer,unit.x,unit.y,scale) &&
            !((unit.flags&ipc::kUnitGroundValid) && pointer_near(pointer,unit.ground_x,unit.ground_y,scale))) continue;
        const float x=unit.x+options.hp_offset_x*scale;
        const float top = unit.y + (options.hp_offset_y-38.0f)*scale;
        if (top < 0.0f || x < 0.0f || x > static_cast<float>(surface.width()) ||
            top > static_cast<float>(surface.height())) {
            continue;
        }
        const bool max_known = std::isfinite(unit.max_hp) && unit.max_hp > 0.0f;
        const float ratio = max_known ? std::clamp(unit.hp / unit.max_hp, 0.0f, 1.0f) : 0.0f;
        const gfx::Color color = !max_known ? kTextColor
            : ratio > 0.55f ? kHpGood : (ratio > 0.25f ? kHpWarn : kHpBad);
        std::wstring text = format_damage(static_cast<std::int64_t>(unit.hp + 0.5f));
        if (max_known && unit.max_hp > unit.hp + 0.5f) {
            text += L"/";
            text += format_damage(static_cast<std::int64_t>(unit.max_hp + 0.5f));
        }
        surface.draw_text(text, x, top, (font_size > 0 ? font_size : 12.0f) * scale,
                          color, gfx::TextAlign::kCenter, true);
    }
}

void draw_operator_stats(gfx::Surface& surface,const ipc::BattleSnapshot& snapshot,float scale,
                         float font_size,const OverlayOptions& options,const PointerState& pointer) {
    if(options.operator_only_paused && !(snapshot.flags&ipc::kBattlePaused)) return;
    const auto totals = statistics::team_metrics(snapshot, options.operator_show_devices);
    const float size=(font_size>0?font_size:13.0f)*scale;
    const float row_height=size*1.35f;
    const auto order=statistics::parse_order(options.operator_order);
    std::vector<int> selected;
    for(int metric:order)
        if((options.operator_metric_mask&(1u<<metric)) && selected.size()<6) selected.push_back(metric);
    for(std::uint32_t i=0;i<std::min(snapshot.operator_count,ipc::kMaxUnits);++i) {
        const auto& actor=snapshot.operators[i];
        if (!statistics::visible(actor, options.operator_show_devices)) continue;
        if((actor.flags&(ipc::kDamageValid|ipc::kDamageScreenValid))!=
            (ipc::kDamageValid|ipc::kDamageScreenValid)) continue;
        if(options.operator_only_hover && !pointer_near(pointer,static_cast<float>(actor.screen.x),
            static_cast<float>(actor.screen.y),scale)) continue;
        const float left=actor.screen.x+options.operator_offset_x*scale;
        const float top=actor.screen.y+options.operator_offset_y*scale;
        float label_width=0;
        for(int metric:selected) label_width=std::max(label_width,estimate_text_width(statistics::kMetrics[metric].label,size));
        float y=top;
        for(int metric:selected) {
            surface.draw_text(statistics::kMetrics[metric].label,left,y,size,kTextColor,gfx::TextAlign::kLeading,true);
            const auto text=(actor.metric_mask&(1u<<metric))?
                statistics::format(statistics::value(actor,metric,totals[ipc::kStatDamage]),metric):L"—";
            surface.draw_text(text,left+label_width+size*.7f,y,size,kTitleColor,gfx::TextAlign::kLeading,true);
            y+=row_height;
        }
    }
}

void draw_enemy_stats(gfx::Surface& surface,const ipc::BattleSnapshot& snapshot,float scale,
                      float font_size,const OverlayOptions& options,const PointerState& pointer) {
    if(!pointer.valid || !snapshot.in_battle || snapshot.battle_state!=2 ||
        (options.enemy_only_paused && !(snapshot.flags&ipc::kBattlePaused))) return;
    const ipc::UnitEntry* nearest=nullptr;
    float best=48.0f*scale*48.0f*scale;
    for(std::uint32_t i=0;i<std::min(snapshot.unit_count,ipc::kMaxUnits);++i) {
        const auto& unit=snapshot.units[i];
        if(!(unit.flags&ipc::kUnitValid) || !unit.tracking_id || !std::isfinite(unit.x) || !std::isfinite(unit.y) ||
            unit.x<0 || unit.y<0 || unit.x>surface.width() || unit.y>surface.height() ||
            ((unit.flags&ipc::kUnitHpValid) && unit.hp<=0)) continue;
        const auto distance=[&](float x,float y) { const auto dx=pointer.x-x,dy=pointer.y-y;return dx*dx+dy*dy; };
        float d=distance(unit.x,unit.y);
        if(unit.flags&ipc::kUnitGroundValid) d=std::min(d,distance(unit.ground_x,unit.ground_y));
        if(d<best || (d==best && (!nearest || unit.tracking_id<nearest->tracking_id))) { best=d;nearest=&unit; }
    }
    if(!nearest) return;
    ipc::EnemyEntry selected{};
    for(std::uint32_t i=0;i<std::min(snapshot.enemy_count,ipc::kMaxEnemies);++i) {
        const auto& enemy=snapshot.enemies[i];
        if(enemy.tracking_id==nearest->tracking_id && (enemy.flags&ipc::kEnemyRuntime)) { selected=enemy;break; }
    }
    // Units cover more instances than the detail roster. Their actual HP remains usable;
    // other missing attributes stay unavailable rather than selecting a more distant enemy.
    const auto hp=[&](float value,std::uint32_t bit,std::int64_t& target) {
        if((nearest->flags&ipc::kUnitHpValid) && std::isfinite(value) && value>=0 &&
            static_cast<double>(value)<static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            target=static_cast<std::int64_t>(std::llround(value));selected.stats_mask|=bit;
        }
    };
    hp(nearest->hp,ipc::kEnemyHp,selected.hp);
    if(nearest->max_hp>0) hp(nearest->max_hp,ipc::kEnemyMaxHp,selected.max_hp);
    const float size=(font_size>0?font_size:13.0f)*scale;
    const auto order=enemy_metrics::parse_order(options.enemy_order);
    std::vector<int> metrics;
    float label_width=0;
    for(int metric:order) if((options.enemy_metric_mask&enemy_metrics::kMetrics[metric].bit) && metrics.size()<6) {
        metrics.push_back(metric);label_width=std::max(label_width,estimate_text_width(enemy_metrics::kMetrics[metric].label,size));
    }
    const float left=nearest->x+options.enemy_offset_x*scale;
    float y=nearest->y+options.enemy_offset_y*scale;
    for(int metric:metrics) {
        surface.draw_text(enemy_metrics::kMetrics[metric].label,left,y,size,kTextColor,gfx::TextAlign::kLeading,true);
        surface.draw_text(enemy_metrics::format(selected,metric),left+label_width+size*.7f,y,size,kTitleColor,gfx::TextAlign::kLeading,true);
        y+=size*1.35f;
    }
}

void draw_enemy_ranges(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                       const FeatureAppearance& appearance) {
    std::array<D2D1_POINT_2F, ipc::kMaxUnits * ipc::kRangePointCount> points{};
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < std::min(snapshot.unit_count, ipc::kMaxUnits); ++i) {
        ipc::Float2 contour[ipc::kRangePointCount]{};
        if (!prepare_range_contour(snapshot.units[i], surface.width(), surface.height(), contour)) {
            continue;
        }
        for (std::uint32_t j = 0; j < ipc::kRangePointCount; ++j) {
            points[count * ipc::kRangePointCount + j] = {contour[j].x, contour[j].y};
        }
        ++count;
    }
    // One union-style low-opacity fill avoids darkening the entire map when ranges overlap.
    surface.draw_polygons(points.data(), count, ipc::kRangePointCount,
                           gfx::Color{1.0f, 0.56f, 0.25f, 0.035f},
                           gfx::Color{1.0f, 0.62f, 0.30f, 0.48f},
                           std::max(1.0f,std::clamp(appearance.line_width,1,8)*scale),appearance.line_style);
}

namespace
{

// 「数值统计」指标编号：0 合计、1 物理、2 法术、3 真实、4 治疗、5 元素。
gfx::Color metric_color(int metric) {
    switch (metric) {
        case 0:
            return gfx::Color{1.0f, 0.86f, 0.42f, 1.0f};  // #FFDB6B 合计
        case 1:
            return gfx::Color{1.0f, 0.42f, 0.42f, 1.0f};
        case 2:
            return gfx::Color{0.31f, 0.66f, 1.0f, 1.0f};
        case 3:
            return gfx::Color{1.0f, 1.0f, 1.0f, 1.0f};
        case 4:
            return gfx::Color{0.42f, 0.88f, 0.54f, 1.0f};
        case 5:
            return gfx::Color{0.75f, 0.55f, 1.0f, 1.0f};
        default:
            return gfx::Color{0.92f, 0.92f, 0.92f, 1.0f};
    }
}

struct DamageAggregate
{
    char actor_id[32] = {};
    float x = 0.0f;
    float y = 0.0f;
    bool screen_valid = false;
    bool any = false;
    double sums[6] = {};  // 与 metric 编号一致；0 为合计
};

struct DamageLine
{
    int metric = 0;
    double value = 0.0;
};

}  // namespace

void draw_damage_numbers(gfx::Surface& surface, const ipc::BattleSnapshot& snapshot, float scale,
                         std::uint32_t metric_mask, bool merged, int sort_metric,
                         const int order[6]) {
    const int count = clamped_damage_count(snapshot);
    if (count <= 0) {
        return;
    }
    std::vector<DamageAggregate> actors;
    actors.reserve(16);
    for (int index = 0; index < count; ++index) {
        const ipc::DamageEntry& entry = snapshot.damage[index];
        if ((entry.flags & ipc::kDamageValid) == 0) {
            continue;
        }
        DamageAggregate* target = nullptr;
        for (DamageAggregate& candidate : actors) {
            if (entry.actor_id[0] != 0 && same_key(candidate.actor_id, entry.actor_id)) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            DamageAggregate fresh;
            for (int i = 0; i < 31 && entry.actor_id[i] != 0; ++i) {
                fresh.actor_id[i] = entry.actor_id[i];
            }
            actors.push_back(fresh);
            target = &actors.back();
        }
        if ((entry.flags & ipc::kDamageScreenValid) != 0 && !target->screen_valid) {
            target->x = static_cast<float>(entry.screen.x);
            target->y = static_cast<float>(entry.screen.y);
            target->screen_valid = true;
        }
        if (entry.damage_type >= ipc::kDamagePhysical && entry.damage_type <= ipc::kDamageElemental) {
            const int metric = static_cast<int>(entry.damage_type);
            const double value = static_cast<double>(entry.total_damage);
            if (value > target->sums[metric]) {
                target->sums[metric] = value;
            }
            target->sums[0] += 0.0;  // 合计在下面统一求
            target->any = true;
        }
    }
    const float size = kDamageSize * scale;
    std::vector<std::pair<std::wstring, gfx::Color>> fallback;
    for (DamageAggregate& actor : actors) {
        if (!actor.any) {
            continue;
        }
        actor.sums[0] = actor.sums[1] + actor.sums[2] + actor.sums[3] + actor.sums[4] + actor.sums[5];
        std::vector<DamageLine> lines;
        if (merged) {
            double total = 0.0;
            if ((metric_mask & 1u) != 0) {
                total = actor.sums[0];
            } else {
                for (int metric = 1; metric < 6; ++metric) {
                    if ((metric_mask & (1u << metric)) != 0) {
                        total += actor.sums[metric];
                    }
                }
            }
            if (total <= 0.0) {
                continue;
            }
            lines.push_back(DamageLine{0, total});
        } else {
            for (int slot = 0; slot < 6; ++slot) {
                const int metric = order[slot] >= 0 && order[slot] < 6 ? order[slot] : slot;
                if ((metric_mask & (1u << slot)) == 0) {
                    continue;
                }
                if (actor.sums[metric] <= 0.0) {
                    continue;
                }
                lines.push_back(DamageLine{metric, actor.sums[metric]});
            }
            if (sort_metric > 0) {
                for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
                    for (std::size_t k = i + 1; k < lines.size(); ++k) {
                        if (lines[k].value > lines[i].value) {
                            const DamageLine tmp = lines[i];
                            lines[i] = lines[k];
                            lines[k] = tmp;
                        }
                    }
                }
            }
        }
        if (lines.empty()) {
            continue;
        }
        const float x = actor.x + kDamageOffsetX * scale;
        const float y = actor.y + kDamageOffsetY * scale;
        const float line_height = size * 1.15f;
        const float start_y = y - (static_cast<float>(lines.size()) - 1.0f) * line_height * 0.5f;
        if (!actor.screen_valid) {
            fallback.emplace_back(format_damage(static_cast<std::int64_t>(actor.sums[0] + 0.5)),
                                  metric_color(lines.front().metric));
            continue;
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::wstring text = format_damage(static_cast<std::int64_t>(lines[i].value + 0.5));
            surface.draw_text(text, x, start_y + static_cast<float>(i) * line_height, size,
                              metric_color(lines[i].metric), gfx::TextAlign::kCenter, true);
        }
    }
    float fallback_y = 10.0f * scale;
    for (const auto& item : fallback) {
        surface.draw_text(item.first, 12.0f * scale, fallback_y, kDamageFallbackSize * scale, item.second,
                          gfx::TextAlign::kLeading, false);
        fallback_y += kFallbackLine * scale;
    }
}

}  // namespace arkhelp::features
