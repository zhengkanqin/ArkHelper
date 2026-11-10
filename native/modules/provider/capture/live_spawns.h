#pragma once
#include "live_data.h"
#include <unordered_set>

namespace ark::arkhelper::capture::live {

struct SpawnSchedule {
    std::vector<Spawn> spawns;
    std::uint32_t serial = 0, revision = 0, frame = 0;
    std::uint64_t sampled_ms = 0;
    double clock_seconds = 0;
    bool available = false;
};

class SpawnReader {
public:
    void clear() { *this = {}; }
    const SpawnSchedule& read(il2cpp::Runtime& rt, void* controller,
                              std::uint32_t serial, std::uint64_t now, std::uint32_t frame) {
        try {
            void* scheduler = member(rt, controller, "_scheduler");
            void* queue = member(rt, scheduler, "m_actionQueue");
            void* waves = member(rt, scheduler, "m_waves");
            double base = 0, version = 0, wave = -1, clock = 0;
            if (!queue || !waves ||
                !number(rt, member(rt, scheduler, "m_fragmentStartTime"), base) ||
                !number(rt, member(rt, scheduler, "m_spawnedWavesCnt"), wave) ||
                !number(rt, member(rt, controller, "userFixedPlayTime"), clock) ||
                !number(rt, member(rt, queue, "_version"), version) ||
                base < 0 || base > 86400 || clock < 0 || clock > 86400 ||
                wave < 0 || wave > 1024 || std::floor(wave) != wave)
                return unavailable(serial, now);
            const int length = rt.count(queue), wave_count = rt.count(waves);
            if (length < 0 || length > 8192 || wave_count < 0 || wave_count > 1024)
                return unavailable(serial, now);
            const auto identity = reinterpret_cast<std::uintptr_t>(queue);
            const auto owner = reinterpret_cast<std::uintptr_t>(scheduler);
            if (schedule_.available && schedule_.serial == serial && identity == queue_ &&
                owner == scheduler_ && waves == waves_ && version == version_ &&
                base == base_ && length == length_ && wave == wave_) {
                schedule_.sampled_ms = now;
                schedule_.clock_seconds = clock; schedule_.frame = frame;
                return schedule_;
            }
            std::vector<Spawn> entries;
            std::vector<void*> data;
            std::vector<double> offsets;
            std::unordered_set<void*> queued;
            for (int i = 0; i < length; ++i) {
                // The queue expands count but retains already executed entries.
                void* item = rt.at(queue, i);
                void* action = member(rt, item, "data");
                double offset = -1;
                if (!action || !number(rt, member(rt, item, "timeOffset"), offset) ||
                    offset < 0 || offset > 86400) continue;
                data.push_back(action); offsets.push_back(offset); queued.insert(action);
            }
            int current_fragment = -1;
            const bool same_wave = schedule_.serial == serial && owner == scheduler_ && waves == waves_ && wave == wave_;
            double fragment_delay = 0;
            std::size_t budget = 0;
            for (int w = static_cast<int>(wave); w < wave_count; ++w) {
                void* fragments = member(rt, rt.at(waves, w), "fragments");
                const int total = fragments ? rt.count(fragments) : 0;
                if (total < 0 || total > 4096) return unavailable(serial, now);
                for (int f = 0; f < total; ++f) {
                    void* fragment = rt.at(fragments, f);
                    void* actions = member(rt, fragment, "actions");
                    const int count = actions ? rt.count(actions) : 0;
                    if (count < 0 || count > 4096 || (budget += count) > 65536)
                        return unavailable(serial, now);
                    std::vector<void*> action_data;
                    bool active = false;
                    for (int a = 0; a < count; ++a) {
                        void* action = rt.at(actions, a);
                        action_data.push_back(action);
                        active |= w == wave && queued.contains(action);
                    }
                    if (active) {
                        if (current_fragment >= 0 ||
                            !number(rt, member(rt, fragment, "preDelay"), fragment_delay) ||
                            fragment_delay < 0 || fragment_delay > 86400)
                            return unavailable(serial, now);
                        current_fragment = f;
                        entries.clear(); // Earlier fragments have been consumed.
                    } else if (!(same_wave && w == wave && f <= last_fragment_)) {
                        for (void* action : action_data) {
                            Spawn spawn;
                            double amount = 0, delay = -1, interval = -1;
                            if (!read_spawn(rt, action, spawn) ||
                                !number(rt, member(rt, action, "count"), amount) ||
                                !number(rt, member(rt, action, "preDelay"), delay) ||
                                !number(rt, member(rt, action, "interval"), interval) ||
                                amount <= 0 || amount > 8192 || std::floor(amount) != amount ||
                                delay < 0 || delay > 86400 || interval < 0 || interval > 86400) continue;
                            bool known = true;
                            if (!optional_bool(rt, action, "managedByScheduler", true, known) || !known)
                                continue;
                            spawn.pending = true;
                            spawn.phase = static_cast<std::uint32_t>((w + 1) * 4096 + f);
                            for (int i = 0; i < static_cast<int>(amount); ++i) {
                                if (entries.size() >= 65536) return unavailable(serial, now);
                                spawn.time = delay + i * interval;
                                entries.push_back(spawn);
                            }
                        }
                    }
                }
            }
            if (current_fragment >= 0) {
                // timeOffset includes preDelay; m_fragmentStartTime already points to
                // its end. Subtract it once, then use the scheduler's user play clock.
                const double origin = base - fragment_delay;
                for (std::size_t i = 0; i < data.size(); ++i) {
                    Spawn spawn;
                    if (!read_spawn(rt, data[i], spawn)) continue;
                    spawn.time = origin + offsets[i];
                    if (spawn.time < 0 || spawn.time > 86400) continue;
                    entries.push_back(std::move(spawn));
                }
            }
            std::stable_sort(entries.begin(), entries.end(), [](const Spawn& a, const Spawn& b) {
                if (a.pending != b.pending) return !a.pending;
                if (a.pending && a.phase != b.phase) return a.phase < b.phase;
                return a.time < b.time;
            });
            queue_ = identity; scheduler_ = owner; waves_ = waves;
            last_fragment_ = current_fragment >= 0 ? current_fragment : (same_wave ? last_fragment_ : -1);
            version_ = version; base_ = base; length_ = length; wave_ = wave;
            schedule_.spawns = std::move(entries);
            schedule_.available = true; schedule_.serial = serial; schedule_.sampled_ms = now;
            schedule_.clock_seconds = clock; schedule_.frame = frame;
            ++schedule_.revision;
        } catch (...) { return unavailable(serial, now); }
        return schedule_;
    }
private:
    static bool read_spawn(il2cpp::Runtime& rt, void* data, Spawn& spawn) {
        if (enum_name(rt, member(rt, data, "actionType")) != "SPAWN") return false;
        bool known = true;
        if (!optional_bool(rt, data, "isValid", true, known) || !known ||
            optional_bool(rt, data, "useExtraRoute", false, known) || !known) return false;
        if (!text(rt, member(rt, data, "randomSpawnGroupKey")).empty() ||
            !text(rt, member(rt, data, "randomSpawnGroupPackKey")).empty()) return false;
        spawn.enemy_key = text(rt, member(rt, data, "key"));
        double route = -1;
        if (spawn.enemy_key.empty() || !number(rt, member(rt, data, "routeIndex"), route) ||
            route < 0 || route >= 65536 || std::floor(route) != route) return false;
        spawn.route_index = static_cast<int>(route);
        return true;
    }
    const SpawnSchedule& unavailable(std::uint32_t serial, std::uint64_t now) {
        schedule_.spawns.clear(); schedule_.available = false;
        schedule_.serial = serial; schedule_.sampled_ms = now; ++schedule_.revision;
        return schedule_;
    }
    SpawnSchedule schedule_;
    std::uintptr_t queue_ = 0, scheduler_ = 0;
    void* waves_ = nullptr; // identity only
    double version_ = 0, base_ = 0, wave_ = -1;
    int length_ = 0, last_fragment_ = -1;
};

inline std::vector<Spawn> spawn_frames(const SpawnSchedule& schedule) {
    auto out = schedule.spawns;
    for (auto& spawn : out) if (!spawn.pending) {
        spawn.frame = static_cast<int>(std::lround(schedule.frame +
            (spawn.time - schedule.clock_seconds) * 30));
    }
    return out;
}
} // namespace ark::arkhelper::capture::live
