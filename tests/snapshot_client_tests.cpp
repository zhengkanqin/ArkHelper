#include <windows.h>
#include "../src/ipc/battle_client.h"
#include "../src/overlay/visibility.h"
#include <cstdio>
#include <string>
#include <stdexcept>

namespace {
unsigned checks = 0;
void check(bool condition,const char* text) { ++checks; if(!condition) throw std::runtime_error(text); }
struct Mapping {
    HANDLE handle = nullptr;
    void* view = nullptr;
    Mapping(const wchar_t* name,DWORD bytes) {
        handle=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,bytes,name);
        view=handle?MapViewOfFile(handle,FILE_MAP_ALL_ACCESS,0,0,bytes):nullptr;
        check(view!=nullptr,"private fixture mapping failed");
    }
    ~Mapping() {if(view)UnmapViewOfFile(view);if(handle)CloseHandle(handle);}
};
}
int main() {
    using namespace arkhelp::ipc;
    try {
        const auto suffix=std::to_wstring(GetCurrentProcessId());
        const auto battle_name=L"Local\\ArkHelper.SnapshotReview."+suffix;
        const auto command_name=L"Local\\ArkHelper.CommandReview."+suffix;
        Mapping battle(battle_name.c_str(),sizeof(BattleSnapshot)),command(command_name.c_str(),sizeof(CommandState));
        auto& state=*static_cast<BattleSnapshot*>(battle.view);
        auto& control=*static_cast<CommandState*>(command.view);
        state={};state.magic=kBattleMagic;state.version=kVersion;state.size=sizeof(state);
        state.battle_serial=3;state.in_battle=1;state.battle_state=2;state.published_ms=GetTickCount64();state.unit_count=1;
        control={};control.magic=kCommandMagic;control.version=kVersion;control.size=sizeof(control);
        control.revision=7;control.feature_mask=kFeatureEnemyPaths;
        BattleClient client(battle_name.c_str());CommandClient commands(command_name.c_str());
        BattleSnapshot out{};CommandState config{};
        check(client.poll(out) && commands.poll(config),"initial coherent snapshots not read");
        const auto published=out.published_ms;
        begin_write(state.sequence);state.unit_count=9;
        check(client.poll(out) && out.unit_count==1 && out.published_ms==published,
            "brief contention reset overlay or refreshed old timestamp");
        state.battle_serial=4;
        check(!client.poll(out),"new battle reused old generation during write");
        end_write(state.sequence);
        check(client.poll(out) && out.unit_count==9 && out.battle_serial==4,"new battle not picked up");
        begin_write(state.sequence);state.in_battle=0;
        check(!client.poll(out),"ended battle reused live overlay cache");
        end_write(state.sequence);check(client.poll(out) && !out.in_battle,"end state not read");
        state.in_battle=1;state.published_ms=GetTickCount64()-2100;
        check(client.poll(out) && !arkhelp::overlay::live_snapshot(out,GetTickCount64()),"old publication freshness bypassed");
        begin_write(state.sequence);Sleep(220);
        check(!client.poll(out),"writer stall extended cache indefinitely");
        end_write(state.sequence);client.close();
        begin_write(state.sequence);check(!client.poll(out),"close/reopen retained previous cache");end_write(state.sequence);
        check(commands.poll(config),"command baseline refresh failed");
        begin_write(control.sequence);
        check(commands.poll(config) && config.feature_mask==kFeatureEnemyPaths,"brief command contention disabled drawings");
        control.revision++;
        check(!commands.poll(config),"changed settings reused old command revision");
        control.feature_mask=0;end_write(control.sequence);
        check(commands.poll(config) && config.feature_mask==0,"feature disable not applied");
        begin_write(control.sequence);Sleep(220);
        check(!commands.poll(config),"command stall retained old revision indefinitely");end_write(control.sequence);
        state.version++;
        check(!client.poll(out),"incompatible mapping accepted cache");
        std::printf("PASS snapshot clients: %u checks, contention/restart/end/stale/version/disable, private IPC only\n",checks);
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
