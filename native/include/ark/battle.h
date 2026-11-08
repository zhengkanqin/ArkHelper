// Battle event service provided by damage_meter.dll.
//
// Consumers declare `damage_meter.dll` as a dependency and acquire
// ARK_BATTLE_SERVICE_NAME through `require_service`.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ARK_BATTLE_ABI_VERSION 2u
#define ARK_BATTLE_SERVICE_NAME "ark.battle.events"
#define ARK_BATTLE_TEST_SERVICE_NAME "ark.battle.test"
#define ARK_MAX_ID 128

enum ArkEventKind : uint32_t {
    ARK_BATTLE_START = 1,
    ARK_DAMAGE = 2,
    ARK_ELEMENTAL = 3, // Elemental buildup, not elemental HP damage (ARK_DAMAGE, type 5).
    ARK_BATTLE_END = 4,
    ARK_CLOCK = 5,
    ARK_HEAL = 6,
    ARK_COST = 7,       // source-attributed effective positive deployment point gain
    ARK_TAKEN = 8,      // actual HP lost by a Character target; distinct from outgoing damage
    ARK_CAPABILITIES = 9, // damage_type carries a bit mask of supported optional event kinds
};

// Matches the game's native damage type numbering.
enum ArkDamageType : int32_t {
    ARK_DAMAGE_NONE = 0,
    ARK_DAMAGE_PHYSICAL = 1,
    ARK_DAMAGE_MAGICAL = 2,
    ARK_DAMAGE_TRUE = 3,
    ARK_DAMAGE_HEAL = 4,
    ARK_DAMAGE_ELEMENTAL = 5,
};

struct ArkEventV1 {
    uint32_t size = sizeof(ArkEventV1);
    uint32_t kind = 0;
    uint64_t sequence = 0;
    uint64_t battle = 0;
    double game_time = 0;
    double calculated_damage = 0;
    double actual_damage = 0;
    int32_t damage_type = 0;
    uint32_t partial = 0;
    char actor_id[ARK_MAX_ID]{};
    char stage_id[ARK_MAX_ID]{};
    uint64_t actor_instance = 0; // Battle-local Unit.instanceUid; zero means unavailable.
};

struct ArkReadStatusV1 {
    uint32_t size = sizeof(ArkReadStatusV1);
    uint32_t partial = 0;
    uint32_t overrun = 0;
    uint32_t ended = 0;
    uint64_t oldest_sequence = 0;
    uint64_t newest_sequence = 0;
};

struct ArkBattleFrameV1 {
    uint32_t size = sizeof(ArkBattleFrameV1);
    uint32_t frame = 0; // Native BattleController.fixedFrameCnt; zero is valid.
    uint64_t battle = 0;
};

struct ArkBattleEventsApiV1 {
    uint32_t size;
    uint32_t abi_version;
    // Copies up to `capacity` (at most 4096) events newer than `after`.
    size_t (*read)(uint64_t after, ArkEventV1* out, size_t capacity, ArkReadStatusV1* status);
    // Cached on the game thread; safe to call from a rendering callback.
    bool (*frame)(ArkBattleFrameV1* out);
};

// Registered only inside the graphical test host.
struct ArkBattleTestApiV1 {
    uint32_t size;
    uint32_t abi_version;
    bool (*publish)(const ArkEventV1* event);
};
