#pragma once
#include "../ui/mouse_relay.h"

namespace arkhelp::launcher {
// The installed agent observes mouse input at its existing integrity level. The
// ordinary UI only requests a short lease and reads events; no input is injected.
class MouseRelayServer {
public:
    ~MouseRelayServer() {
        if(worker_) {
            PostThreadMessageW(thread_,WM_QUIT,0,0);
            WaitForSingleObject(worker_,INFINITE);CloseHandle(worker_);
        }
        if(state_)UnmapViewOfFile(state_);
        if(mapping_)CloseHandle(mapping_);
    }
    bool start(SECURITY_ATTRIBUTES* security) {
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,security,PAGE_READWRITE,0,sizeof(MouseRelayState),kMouseRelayName);
        if(!mapping_)return false;
        state_=static_cast<MouseRelayState*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(MouseRelayState)));
        if(!state_)return false;
        *state_=MouseRelayState{};
        state_->epoch=(GetTickCount64()<<16)^GetCurrentProcessId();state_->magic=kMouseRelayMagic;
        worker_=CreateThread(nullptr,0,run,this,0,&thread_);
        return worker_!=nullptr;
    }
private:
    HANDLE mapping_=nullptr,worker_=nullptr;
    MouseRelayState* state_=nullptr;
    DWORD thread_=0;
    HWND window_=nullptr;
    SharedMouseInput input_;
    void publish() {
        auto events=input_.take();
        InterlockedIncrement(&state_->sequence);
        for(const auto& event:events) {
            auto& target=state_->events[state_->written%kMouseRelayCapacity];
            target={state_->written+1,event.time,reinterpret_cast<UINT_PTR>(event.foreground),
                event.screen.x,event.screen.y,event.message,static_cast<std::uint32_t>(event.keys)};
            ++state_->written;
        }
        state_->published=input_.running()?GetTickCount64():0;
        MemoryBarrier();InterlockedIncrement(&state_->sequence);
    }
    void tick() {
        const auto now=GetTickCount64();
        const auto lease=static_cast<ULONGLONG>(InterlockedCompareExchange64(&state_->requested_until,0,0));
        HANDLE app=OpenMutexW(SYNCHRONIZE,FALSE,kAppMutex);
        const bool observe=app && lease>now && lease-now<=2000 && !app_stopping();
        if(app)CloseHandle(app);
        if(observe && !input_.running())input_.start(window_,WM_APP+1,true);
        if(!observe)input_.stop();
        publish();
    }
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<MouseRelayServer*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<MouseRelayServer*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(self && message==WM_TIMER){self->tick();return 0;}
        if(self && message==WM_APP+1){self->publish();return 0;}
        return DefWindowProcW(window,message,wparam,lparam);
    }
    static DWORD WINAPI run(void* context) {
        auto& self=*static_cast<MouseRelayServer*>(context);
        WNDCLASSW klass{};klass.hInstance=GetModuleHandleW(nullptr);klass.lpfnWndProc=procedure;
        klass.lpszClassName=L"ArkHelper.AgentMouse";RegisterClassW(&klass);
        self.window_=CreateWindowExW(0,klass.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,klass.hInstance,&self);
        if(!self.window_)return 1;
        SetTimer(self.window_,1,100,nullptr);
        MSG message{};
        while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        self.input_.stop();self.publish();
        KillTimer(self.window_,1);DestroyWindow(self.window_);self.window_=nullptr;
        return 0;
    }
};
} // namespace arkhelp::launcher
