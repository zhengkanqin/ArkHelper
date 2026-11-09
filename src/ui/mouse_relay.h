#pragma once
#include "shared_mouse_input.h"
#include <cstdint>
#include <cstring>

namespace arkhelp {
#ifndef ARKHELP_MOUSE_RELAY_SUFFIX
#define ARKHELP_MOUSE_RELAY_SUFFIX L""
#endif
inline constexpr wchar_t kMouseRelayName[]=L"Local\\ArkHelp.MouseRelay.v1" ARKHELP_MOUSE_RELAY_SUFFIX;
inline constexpr std::uint32_t kMouseRelayMagic=0x41484D31;
inline constexpr unsigned kMouseRelayCapacity=256;
struct MouseRelayEvent {
    std::uint64_t serial=0,time=0,foreground=0;
    std::int32_t x=0,y=0;
    std::uint32_t message=0,keys=0;
};
struct MouseRelayState {
    std::uint32_t magic=0,version=1,size=sizeof(MouseRelayState);
    volatile LONG sequence=0;
    std::uint64_t epoch=0,published=0;
    alignas(8) volatile LONG64 requested_until=0; // ordinary UI's short observation lease
    std::uint64_t written=0;
    MouseRelayEvent events[kMouseRelayCapacity]{};
};

class MouseRelayReader {
public:
    explicit MouseRelayReader(const wchar_t* name=kMouseRelayName):name_(name){}
    ~MouseRelayReader(){close();}
    void close() {
        if(state_)UnmapViewOfFile(state_);
        if(mapping_)CloseHandle(mapping_);
        state_=nullptr;mapping_=nullptr;epoch_=cursor_=healthy_until_=0;
    }
    template<class Deliver>
    bool poll(ULONGLONG now,Deliver&& deliver) {
        if(!state_ && now>=retry_) {
            retry_=now+1000;
            mapping_=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name_);
            if(mapping_)state_=static_cast<MouseRelayState*>(MapViewOfFile(mapping_,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(MouseRelayState)));
            if(!state_ && mapping_){CloseHandle(mapping_);mapping_=nullptr;}
        }
        if(!state_)return false;
        if(state_->magic!=kMouseRelayMagic || state_->version!=1 || state_->size!=sizeof(MouseRelayState)) {
            close();return false;
        }
        InterlockedExchange64(&state_->requested_until,static_cast<LONG64>(now+1500));
        MouseRelayState snapshot;
        bool coherent=false;
        for(int attempt=0;attempt<3;++attempt) {
            const auto first=InterlockedCompareExchange(&state_->sequence,0,0);
            if(first&1)continue;
            std::memcpy(&snapshot,state_,sizeof(snapshot));MemoryBarrier();
            if(first==InterlockedCompareExchange(&state_->sequence,0,0)){coherent=true;break;}
        }
        if(!coherent)return now<=healthy_until_;
        const auto copied_at=GetTickCount64();
        if(copied_at>now)now=copied_at;
        if(!snapshot.published || snapshot.published>now || now-snapshot.published>1000) {
            if(epoch_)deliver(SharedMouseEvent{WM_CANCELMODE});
            epoch_=cursor_=healthy_until_=0;return false;
        }
        healthy_until_=now+200;
        if(epoch_!=snapshot.epoch || snapshot.written<cursor_) {
            deliver(SharedMouseEvent{WM_CANCELMODE});
            epoch_=snapshot.epoch;cursor_=snapshot.written;
            return true;
        }
        if(snapshot.written-cursor_>kMouseRelayCapacity) {
            deliver(SharedMouseEvent{WM_CANCELMODE});cursor_=snapshot.written;
        }
        while(cursor_<snapshot.written) {
            const auto& item=snapshot.events[cursor_%kMouseRelayCapacity];
            ++cursor_;
            if(item.serial!=cursor_ || !item.time || item.time>now || now-item.time>1000) {
                deliver(SharedMouseEvent{WM_CANCELMODE});continue;
            }
            deliver(SharedMouseEvent{item.message,{item.x,item.y},item.keys,item.time,
                reinterpret_cast<HWND>(static_cast<UINT_PTR>(item.foreground))});
        }
        return true;
    }
private:
    const wchar_t* name_;
    HANDLE mapping_=nullptr;
    MouseRelayState* state_=nullptr;
    std::uint64_t epoch_=0,cursor_=0,healthy_until_=0,retry_=0;
};
} // namespace arkhelp
