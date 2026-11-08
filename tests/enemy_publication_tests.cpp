#include "../native/modules/provider/ipc/enemy_snapshot.h"
#include <cstdio>
#include <cstring>
#include <limits>

int main() {
    namespace ipc = arkhelp::ipc;
    using ark::arkhelper::capture::EnemySample;
    using ark::arkhelper::ipc_publish::make_enemy_entry;
    EnemySample a{};
    a.tracking_id = 100;
    std::strcpy(a.enemy_key, "same_enemy");
    a.hp = 2200;
    a.max_hp = 4000;
    a.atk = 600;
    a.def = 0;
    a.magic_resistance = -12.5;
    a.stats_mask = ipc::kEnemyHp | ipc::kEnemyMaxHp | ipc::kEnemyAtk |
                   ipc::kEnemyDef | ipc::kEnemyMagicResistance;
    const auto one = make_enemy_entry(a);
    a.tracking_id = 101;
    a.max_hp = 12000;
    a.atk = 1250;
    const auto two = make_enemy_entry(a);
    if (one.tracking_id == two.tracking_id || one.max_hp != 4000 || two.max_hp != 12000 ||
        one.atk != 600 || two.atk != 1250 || one.magic_resistance != -12500 ||
        one.def != 0 || !(one.stats_mask & ipc::kEnemyDef) ||
        !(one.flags & ipc::kEnemyRuntime) || (one.flags & ipc::kEnemyReference)) { return 1; }
    // Same enemy key keeps distinct per-instance stats; unavailable is not confused with zero.
    a.stats_mask = ipc::kEnemyHp;
    a.atk = 9000;
    const auto partial = make_enemy_entry(a);
    if (partial.stats_mask != ipc::kEnemyHp || partial.atk != 0 || partial.max_hp != 0) { return 2; }
    a.stats_mask = ipc::kEnemyHp | ipc::kEnemyMaxHp | ipc::kEnemyAtk | ipc::kEnemyMoveSpeed;
    a.hp = std::numeric_limits<double>::quiet_NaN();
    a.max_hp = std::numeric_limits<double>::infinity();
    a.atk = 1e200;
    a.move_speed = 1.234;
    const auto invalid = make_enemy_entry(a);
    if (invalid.stats_mask != ipc::kEnemyMoveSpeed || invalid.move_speed != 1234) { return 3; }
    std::memset(a.enemy_name, 'X', sizeof(a.enemy_name));
    const auto bounded = make_enemy_entry(a);
    if (bounded.enemy_name[ipc::kNameCapacity - 1] != 0) { return 4; }
    std::puts("PASS live enemy publication: distinct instance modifiers, availability, numeric and string bounds");
    return 0;
}
