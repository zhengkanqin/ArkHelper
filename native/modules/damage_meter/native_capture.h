#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <ark/plugin.h>

#include "event_history.h"

namespace ark::damage_meter {

// Publishes battle events by hooking the game's BattleLogger and BattleController methods
// through the host's native hook API. Also pumps the host's game-thread task queue from the
// game's global update so that queued tasks run on the native main thread.
class NativeCapture {
public:
    NativeCapture(EventHistory& history, const ArkPluginApiV1& host);
    // Must only run after shutdown() succeeded and SDK tasks have drained.
    ~NativeCapture();

    NativeCapture(const NativeCapture&) = delete;
    NativeCapture& operator=(const NativeCapture&) = delete;

    // Returns false while the IL2CPP runtime is not ready yet; throws on incompatible game
    // metadata or hook failures (after rolling back via shutdown()).
    bool initialize();
    // Removes all hooks and waits for detours in flight. Returns false when a hook could not
    // be removed; the call must then be retried later.
    bool shutdown();

private:
    struct Bindings;
    class CallbackScope;

    void begin_battle(void* controller, bool partial);
    void end_battle();
    void update_battle(void* controller);
    void record_damage(void* logger, void* modifier, bool elemental);
    void record_cost(void* controller, void* source, int amount);
    double game_clock();
    void publish_frame();

    static void on_log_modifier(void* self, void* modifier, void* method);
    static void on_log_ep_modifier(void* self, void* modifier, void* method);
    static void on_start_game(void* self, void* method);
    static void on_clear_runtime_data(void* self, void* method);
    static void on_destroy(void* self, void* method);
    static void on_update(void* self, void* method);
    static void on_global_update(void* self, void* method);
    static bool on_modify_cost(void* self, int value, void* source, int side,
                               bool display, bool negative, bool retrigger,
                               bool cast_skill, bool deploy_delta, void* method);
    static bool on_withdraw(void* self, bool dead_state, bool force,
                            bool manual, bool log, void* method);
    static void on_withdraw_by_level(void* self, void* method);

    std::unique_ptr<Bindings> bindings_;
    EventHistory& history_;
    const ArkPluginApiV1& host_;

    std::recursive_mutex lifecycle_mutex_;
    std::mutex callback_mutex_;
    // Serializes task pumping against shutdown so no task starts after host_active_ clears.
    std::mutex host_dispatch_mutex_;
    std::condition_variable callbacks_drained_;
    std::size_t callbacks_in_flight_ = 0; // Guarded by callback_mutex_.
    bool callbacks_enabled_ = false;      // Guarded by callback_mutex_.
    std::atomic<bool> ready_{false};
    std::atomic<bool> host_active_{true};

    // Game-thread battle tracking state.
    bool battle_active_ = false;
    bool battle_ended_ = false;
    double last_clock_ = -1;
    double observed_clock_ = -1;
    void* controller_ = nullptr;
    void* character_class_ = nullptr;
    void* battle_controller_class_ = nullptr;
    std::size_t real_delta_offset_ = 0;
    std::uint32_t capabilities_ = 0;

    std::vector<std::uint64_t> hooks_;
};

} // namespace ark::damage_meter
