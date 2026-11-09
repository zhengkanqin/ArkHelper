#include "../src/ui/mouse_relay.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    using namespace arkhelp;
    HANDLE mapping=nullptr;MouseRelayState* state=nullptr;
    unsigned checks=0;
    const auto check=[&](bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);};
    try {
        const auto name=L"Local\\ArkHelper.MouseRelay.Test."+std::to_wstring(GetCurrentProcessId());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(MouseRelayState),name.c_str());
        state=mapping?static_cast<MouseRelayState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(MouseRelayState))):nullptr;
        check(state!=nullptr,"private mouse fixture unavailable");
        *state=MouseRelayState{};state->magic=kMouseRelayMagic;state->epoch=99;
        auto now=GetTickCount64();
        std::vector<SharedMouseEvent> events;
        auto receive=[&](const SharedMouseEvent& event){events.push_back(event);};
        MouseRelayReader reader(name.c_str());
        check(!reader.poll(now,receive) && state->requested_until==static_cast<LONG64>(now+1500),"missing producer heartbeat accepted or lease not written");
        state->published=now;
        check(reader.poll(now,receive) && events.size()==1 && events[0].message==WM_CANCELMODE,"initial connection did not reset old gestures");
        events.clear();
        const auto append=[&](UINT message,int x,int y,unsigned keys) {
            auto& entry=state->events[state->written%kMouseRelayCapacity];
            entry={state->written+1,now,0x1234,x,y,message,keys};++state->written;
        };
        append(WM_LBUTTONDOWN,200,300,MK_LBUTTON);
        append(WM_LBUTTONUP,200,300,0);
        append(WM_MOUSEMOVE,500,600,0);
        check(reader.poll(now,receive) && events.size()==3,"fast down/up/move not preserved");
        check(events[0].screen.x==200 && events[2].screen.x==500 && events[0].foreground==reinterpret_cast<HWND>(0x1234),"historical click position or foreground lost");
        reader.poll(now,receive);check(events.size()==3,"mouse events duplicated");
        events.clear();state->written+=kMouseRelayCapacity+1;
        check(reader.poll(now,receive) && events.size()==1 && events[0].message==WM_CANCELMODE,"overflow did not cancel gesture");
        events.clear();state->epoch++;
        check(reader.poll(now,receive) && events.size()==1,"agent restart did not reset observation cursor");
        events.clear();InterlockedIncrement(&state->sequence);
        check(reader.poll(now+10,receive) && events.empty(),"brief writer contention switched to a duplicate local source");
        check(!reader.poll(now+250,receive),"writer stall retained an active source forever");
        InterlockedIncrement(&state->sequence);
        check(!reader.poll(now+1100,receive),"stale heartbeat accepted");
        state->published=now+1100;append(WM_LBUTTONDOWN,200,300,MK_LBUTTON);events.clear();
        check(reader.poll(now+1100,receive) && events.size()==1 && events[0].message==WM_CANCELMODE,"reconnection replayed stale click");
        reader.close();UnmapViewOfFile(state);state=nullptr;CloseHandle(mapping);mapping=nullptr;
        std::printf("PASS mouse relay: %u checks; private IPC, leases, ordered quick clicks, restart/overflow/stale/duplicate guards\n",checks);return 0;
    } catch(const std::exception& e) {
        if(state)UnmapViewOfFile(state);if(mapping)CloseHandle(mapping);
        std::fprintf(stderr,"FAIL mouse relay: %s\n",e.what());return 1;
    }
}
