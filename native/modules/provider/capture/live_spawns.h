#pragma once
#include "live_data.h"

namespace ark::arkhelper::capture::live {

struct SpawnSchedule {
    std::vector<Spawn> spawns;
    std::uint32_t serial = 0, revision = 0;
    std::uint64_t sampled_ms = 0;
    bool available = false;
};

class SpawnReader {
public:
    void clear() { *this = {}; }
    const SpawnSchedule& read(il2cpp::Runtime& rt, void* controller,
                              std::uint32_t serial, std::uint64_t now) {
        try {
            void* scheduler = member(rt, controller, "_scheduler");
            void* queue = member(rt, scheduler, "m_actionQueue");
            double base = 0, version = 0;
            if (!queue || !number(rt, member(rt, scheduler, "m_fragmentStartTime"), base) ||
                !number(rt, member(rt, queue, "_version"), version) || base < 0 || base > 86400)
                return unavailable(serial, now);
            const int length = rt.count(queue);
            if (length < 0 || length > 8192) return unavailable(serial, now);
            const auto identity = reinterpret_cast<std::uintptr_t>(queue);
            const auto owner = reinterpret_cast<std::uintptr_t>(scheduler);
            if (schedule_.available && schedule_.serial == serial && identity == queue_ &&
                owner == scheduler_ && version == version_ && base == base_ && length == length_) {
                schedule_.sampled_ms = now;
                return schedule_;
            }
            std::vector<Spawn> entries;
            entries.reserve(length);
            for (int i = 0; i < length; ++i) {
                // ActionItem is a value type; Runtime::at invokes List<T>.get_Item
                // and boxes it. The queue has already expanded ActionData.count.
                void* item = rt.at(queue, i);
                void* data = member(rt, item, "data");
                if (enum_name(rt, member(rt, data, "actionType")) != "SPAWN") continue;
                bool known = true;
                if (!optional_bool(rt, data, "isValid", true, known) || !known) continue;
                if (optional_bool(rt, data, "useExtraRoute", false, known) || !known) continue;
                if (!text(rt, member(rt, data, "randomSpawnGroupKey")).empty() ||
                    !text(rt, member(rt, data, "randomSpawnGroupPackKey")).empty()) continue;
                Spawn spawn;
                spawn.enemy_key = text(rt, member(rt, data, "key"));
                double offset = 0, route = -1;
                if (spawn.enemy_key.empty() ||
                    !number(rt, member(rt, item, "timeOffset"), offset) ||
                    !number(rt, member(rt, data, "routeIndex"), route) ||
                    offset < 0 || route < 0 || route > 65536 || std::floor(route) != route) continue;
                spawn.time = base + offset;
                if (spawn.time > 86400) continue;
                spawn.frame = static_cast<int>(std::lround(spawn.time * 30));
                spawn.route_index = static_cast<int>(route);
                entries.push_back(std::move(spawn));
            }
            std::stable_sort(entries.begin(), entries.end(), [](const Spawn& a, const Spawn& b) {
                return a.time < b.time;
            });
            queue_ = identity; scheduler_ = owner; version_ = version; base_ = base; length_ = length;
            schedule_.spawns = std::move(entries);
            schedule_.available = true; schedule_.serial = serial; schedule_.sampled_ms = now;
            ++schedule_.revision;
        } catch (...) { return unavailable(serial, now); }
        return schedule_;
    }
private:
    const SpawnSchedule& unavailable(std::uint32_t serial, std::uint64_t now) {
        schedule_.spawns.clear(); schedule_.available = false;
        schedule_.serial = serial; schedule_.sampled_ms = now; ++schedule_.revision;
        return schedule_;
    }
    SpawnSchedule schedule_;
    std::uintptr_t queue_ = 0, scheduler_ = 0;
    double version_ = 0, base_ = 0;
    int length_ = 0;
};
} // namespace ark::arkhelper::capture::live
