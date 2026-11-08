#include "../native/modules/provider/capture/path_geometry.h"
#include "../native/modules/provider/capture/operator_samples.h"
#include "../native/modules/provider/ipc/operator_publication.h"
#include <cstdio>
#include <limits>

namespace pg = ark::arkhelper::capture::path_geometry;
namespace cap = ark::arkhelper::capture;
namespace ipc = arkhelp::ipc;
namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", description); }
}
}
int main() {
    pg::Route route;
    check(route.append({0,0}, true) && route.append({1,0}) && route.append({2,0}) &&
          route.count == 2 && route.points[1].point.x == 2, "exact straight runs merge without losing endpoint");
    route.append({2,1});
    check(route.count == 3 && route.points[1].point.x == 2 && route.points[1].point.y == 0,
          "route keeps a real turn");
    route.append({7,1}, true);
    route.append({8,1});
    check(route.count == 5 && (route.points[3].flags & ipc::kPathMoveTo), "teleport starts a disconnected segment");
    route.append({7,1});
    check(route.count == 6, "backtracking is not simplified across a reversal");
    const auto old_count = route.count;
    check(!route.append({std::numeric_limits<float>::quiet_NaN(),0}) && route.count == old_count,
          "invalid coordinate never enters a route");
    pg::Route long_route;
    for (unsigned i = 0; i < ipc::kMaxPathPoints; ++i) long_route.append({float(i),float(i%2)});
    check(!long_route.append({100,3}) && long_route.count == ipc::kMaxPathPoints && long_route.truncated,
          "capacity marks prefix truncated and preserves true geometry");
    pg::GridHistory history;
    check(history.visit({1,2}) && history.visit({2,2}) && !history.visit({1,2}), "runtime next-map cycles are rejected");
    pg::GridHistory bounded;
    bool all = true;
    for (int i = 0; i < 1024; ++i) all &= bounded.visit({i,0});
    check(all && !bounded.visit({1024,0}), "node traversal has a strict maximum");

    cap::OperatorSample operators[ipc::kMaxUnits]{};
    std::uint32_t count = 0;
    cap::operator_samples::append(operators,count,ipc::kMaxUnits,"char_a",.25f,.75f);
    cap::operator_samples::append(operators,count,ipc::kMaxUnits,"char_b",.5f,.5f);
    check(count == 2 && operators[0].flags == 1, "two live characters retain their actor IDs");
    cap::operator_samples::append(operators,count,ipc::kMaxUnits,"char_a",.75f,.25f);
    check(count == 2 && operators[0].flags == 0, "duplicate live actor IDs disable ambiguous anchor");
    cap::operator_samples::append(operators,count,ipc::kMaxUnits,"char_a",.8f,.2f);
    check(operators[0].flags == 0, "third duplicate never restores a previously ambiguous anchor");

    ipc::BattleSnapshot snapshot;
    snapshot.damage_count = 1;
    std::strcpy(snapshot.damage[0].actor_id,"char_a");
    snapshot.damage[0].flags = ipc::kDamageValid | ipc::kDamageScreenValid;
    snapshot.damage[0].metrics[ipc::kStatDamage] = 123;
    snapshot.team_metrics[ipc::kStatDamage] = 123;
    const auto supported = (1u << ipc::kStatDamage) | (1u << ipc::kStatHeal);
    ark::arkhelper::ipc_publish::attach_operator_positions(snapshot,operators,count,supported,1000,500);
    check(snapshot.damage_count == 2 && !(snapshot.damage[0].flags & ipc::kDamageScreenValid),
          "ambiguous aggregate remains in report with no screen anchor");
    check(!std::strcmp(snapshot.damage[1].actor_id,"char_b") &&
          (snapshot.damage[1].flags & ipc::kDamageScreenValid) &&
          snapshot.damage[1].screen.x == 500 && snapshot.damage[1].screen.y == 250,
          "zero-output active operator gets the correct screen position");
    check(snapshot.damage[1].metric_mask == supported && snapshot.damage[1].metrics[ipc::kStatDamage] == 0 &&
          !(snapshot.damage[1].metric_mask & (1u << ipc::kStatCost)),
          "zero-output row preserves actual capability availability");
    check(snapshot.team_metrics[ipc::kStatDamage] == 123 && snapshot.damage[0].metrics[ipc::kStatDamage] == 123,
          "adding active zero-output rows never changes accumulated totals");
    ark::arkhelper::ipc_publish::attach_operator_positions(snapshot,nullptr,0,supported,1000,500);
    check(!(snapshot.damage[1].flags & ipc::kDamageScreenValid) && snapshot.damage_count == 2,
          "departed operators keep report rows and immediately lose their anchors");
    operators[1].viewport_x = std::numeric_limits<float>::infinity();
    ark::arkhelper::ipc_publish::attach_operator_positions(snapshot,operators,count,supported,1000,500);
    check(!(snapshot.damage[1].flags & ipc::kDamageScreenValid), "nonfinite projection is hidden");
    std::printf("Path capture: %u/%u passed; BattleSnapshot=%zu CommandState=%zu\n",checks-failures,checks,
                sizeof(ipc::BattleSnapshot),sizeof(ipc::CommandState));
    return failures ? 1 : 0;
}
