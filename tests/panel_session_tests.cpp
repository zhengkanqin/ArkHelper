#include <cstdio>
#include <stdexcept>
#include <string>

#include "../src/data/panel_session.h"

using arkhelp::PanelSession;
namespace ipc = arkhelp::ipc;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
ipc::BattleSnapshot sample(std::uint64_t now, std::uint32_t serial=1) {
    ipc::BattleSnapshot result;
    result.published_ms=now;result.battle_serial=serial;result.in_battle=1;
    result.battle_state=2;result.frame=100;result.time_seconds=10;
    std::snprintf(result.level_id,sizeof(result.level_id),"stage_%u",serial);
    result.enemy_count=1;result.enemies[0].hp=350;result.enemies[0].max_hp=1000;
    result.spawn_count=1;result.spawns[0].count=3;result.spawns[0].remaining_seconds=12;
    result.damage_count=1;result.damage[0].metrics[ipc::kStatDamage]=650;
    result.team_metrics[ipc::kStatDamage]=650;
    return result;
}
void first_launch_and_retention() {
    PanelSession state;
    check(!state.visible(ipc::kFeatureWiki),"first launch showed a panel");
    auto idle=sample(1000);idle.in_battle=0;idle.battle_state=3;
    state.observe(&idle,1000);
    check(state.phase()==PanelSession::Phase::kIdle,"old completed snapshot appeared on startup");
    idle.battle_state=-1;state.observe(&idle,1000);
    check(!state.visible(ipc::kFeatureDamageNumbers),"homepage stale totals appeared on startup");
    idle.in_battle=1;idle.battle_state=-1;state.observe(&idle,1000);
    check(state.phase()==PanelSession::Phase::kIdle,"unknown or loading state appeared as a battle");
    auto live=sample(1200);state.observe(&live,1200);
    check(state.phase()==PanelSession::Phase::kLive && state.visible(ipc::kFeatureWiki),"battle did not show enabled panels");
    auto ended=live;ended.published_ms=1400;ended.battle_state=3;
    ended.enemy_count=ended.spawn_count=0;
    ended.team_metrics[ipc::kStatDamage]=700;ended.damage[0].metrics[ipc::kStatDamage]=700;
    state.observe(&ended,1400);
    check(state.retained(),"completed battle was not retained");
    check(state.snapshot().enemy_count==1 && state.snapshot().enemies[0].hp==350,"teardown discarded the last enemy data");
    check(state.snapshot().spawn_count==1 && state.snapshot().team_metrics[ipc::kStatDamage]==700,
          "retained plan or final statistics were lost");
    idle={};idle.published_ms=1500;idle.battle_serial=1;idle.battle_state=-1;
    state.observe(&idle,1500);
    check(state.snapshot().time_seconds==10 && state.snapshot().enemy_count==1,"homepage empty snapshot replaced final report");
    idle.damage_count=1;idle.damage[0].metrics[ipc::kStatDamage]=730;idle.team_metrics[ipc::kStatDamage]=730;
    state.observe(&idle,1500);
    check(state.snapshot().team_metrics[ipc::kStatDamage]==730 && state.snapshot().enemy_count==1,
          "late final statistics did not merge with frozen battle details");
    state.dismiss(ipc::kFeatureDamageNumbers);state.observe(&idle,1500);
    check(!state.visible(ipc::kFeatureDamageNumbers) && state.visible(ipc::kFeatureWiki),"dismissal affected another panel or was undone");
    auto next=sample(1800,2);next.enemy_count=0;state.observe(&next,1800);
    check(state.visible(ipc::kFeatureDamageNumbers) && state.phase()==PanelSession::Phase::kLive,
          "next battle did not restore the dismissed panel");
    check(state.snapshot().enemy_count==0,"previous enemy data leaked into a new battle");
}
void disconnection_is_not_completion() {
    PanelSession state;
    auto live=sample(1000);state.observe(&live,1000);
    state.observe(nullptr,1100);
    check(state.phase()==PanelSession::Phase::kInterrupted && state.snapshot().enemy_count==1,
          "read failure was treated as completion or discarded data");
    live.published_ms=1200;live.frame=101;state.observe(&live,1200);
    check(state.phase()==PanelSession::Phase::kLive,"same-battle connection recovery failed");
    state.observe(&live,5001);
    check(state.phase()==PanelSession::Phase::kInterrupted,"expired shared memory appeared live");
    live.published_ms=5100;live.flags=ipc::kBattleStale;state.observe(&live,5100);
    check(state.phase()==PanelSession::Phase::kInterrupted,"capture stale flag appeared live");
    state.dismiss(ipc::kFeatureWiki);
    check(!state.visible(ipc::kFeatureWiki),"interrupted report could not be dismissed");
    live.flags=0;live.battle_serial=2;live.frame=0;state.observe(&live,5100);
    check(state.visible(ipc::kFeatureWiki) && state.phase()==PanelSession::Phase::kLive,
          "next battle did not reset interrupted dismissal");
}
void width_revision_is_one_shot() {
    arkhelp::PanelWidthRevision revision;
    check(revision.changed(0),"initial content did not request one fit");
    for(int poll=0;poll<100;++poll) check(!revision.changed(0),"unchanged content overrode a manual resize");
    check(revision.changed(1) && !revision.changed(1),"opening PRTS did not fit exactly once");
    check(revision.changed(3) && !revision.changed(3),"loaded PRTS did not fit exactly once");
    check(revision.changed(0) && !revision.changed(0),"closing PRTS did not fit exactly once");
    revision={};check(revision.changed(0),"explicit feature enable could not request a new fit");
}
void reference_width_persistence() {
    arkhelp::PanelAutoWidth width;
    check(!width.take(false), "unloaded PRTS consumed its initial fit");
    check(width.take(true) && !width.take(true), "initial PRTS width did not fit once");
    width.restore(510);
    for (int toggle = 0; toggle < 5; ++toggle) {
        check(!width.take(false) && !width.take(true), "reopening PRTS replaced its saved width");
    }
    arkhelp::PanelAutoWidth restarted;
    restarted.restore(510);
    check(!restarted.take(true), "restart replaced a persisted PRTS width");
    restarted.request();
    check(!restarted.take(false) && restarted.take(true) && !restarted.take(true),
          "explicit layout change did not wait for content and fit once");
    restarted.request();
    restarted.restore(620);
    check(!restarted.take(true), "late content replaced a resize performed while loading");
    restarted.restore(0);
    check(restarted.take(true), "reset did not restore automatic fitting");
}
void last_valid_enemy_values() {
    PanelSession state;
    auto live=sample(1000);
    auto& enemy=live.enemies[0];
    enemy.flags=ipc::kEnemyValid|ipc::kEnemyRuntime;enemy.tracking_id=42;
    std::snprintf(enemy.enemy_key,sizeof(enemy.enemy_key),"enemy_same_type");
    enemy.atk=780;enemy.stats_mask=ipc::kEnemyHp|ipc::kEnemyMaxHp|ipc::kEnemyAtk;
    state.observe(&live,1000);
    live.published_ms=1100;enemy.hp=0;enemy.atk=0;enemy.stats_mask=ipc::kEnemyHp;
    state.observe(&live,1100);
    check(state.snapshot().enemies[0].hp==0 && state.snapshot().enemies[0].atk==780 &&
          (state.snapshot().enemies[0].stats_mask&ipc::kEnemyAtk),
          "temporary missing attributes erased known values or restored nonzero HP after death");
    live.published_ms=1200;live.battle_state=3;enemy={};enemy.flags=ipc::kEnemyReference;
    state.observe(&live,1200);
    check(state.retained() && state.snapshot().enemies[0].tracking_id==42 && state.snapshot().enemies[0].atk==780,
          "teardown reference roster replaced last measured runtime attributes");
    live=sample(1300,2);live.enemies[0].tracking_id=42;live.enemies[0].flags=ipc::kEnemyRuntime;
    state.observe(&live,1300);
    check(!(state.snapshot().enemies[0].stats_mask&ipc::kEnemyAtk),"last battle values leaked into a new battle");
    live=sample(1400,3);live.enemies[0]=state.snapshot().enemies[0];
    live.enemies[0].stats_mask=ipc::kEnemyAtk;live.enemies[0].atk=900;
    std::snprintf(live.enemies[0].enemy_key,sizeof(live.enemies[0].enemy_key),"enemy_same_type");
    state.observe(&live,1400);
    live.published_ms=1500;live.enemies[0].tracking_id=43;live.enemies[0].stats_mask=0;
    state.observe(&live,1500);
    check(!(state.snapshot().enemies[0].stats_mask&ipc::kEnemyAtk),"same-type distinct instance inherited another enemy's stats");
    live.published_ms=1600;live.enemy_count=0;state.observe(&live,1600);
    check(state.snapshot().enemy_count==1,"empty live sample erased the last inspectable enemy");
}
}
int main() {
    try {
        first_launch_and_retention();std::puts("PASS startup hiding, final report retention, dismissal and next battle");
        disconnection_is_not_completion();std::puts("PASS connection interruption, stale data and recovery");
        width_revision_is_one_shot();std::puts("PASS one-shot fitting preserves manual width between content changes");
        reference_width_persistence();std::puts("PASS PRTS width across toggle, restart, delayed content and layout changes");
        last_valid_enemy_values();std::puts("PASS sparse enemy attributes, zero HP, teardown reference fallback and battle/instance isolation");
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what());return 1; }
    return 0;
}
