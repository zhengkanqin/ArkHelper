#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace arkhelp {
struct Shortcut {
    std::array<std::uint8_t,8> keys{};
    bool empty() const { return !keys[0]; }
    bool operator==(const Shortcut&) const = default;
};
inline unsigned shortcut_key(unsigned key) {
    if(key==0xa0 || key==0xa1)return 0x10;
    if(key==0xa2 || key==0xa3)return 0x11;
    if(key==0xa4 || key==0xa5)return 0x12;
    if(key==0x5c)return 0x5b;
    return key;
}
inline Shortcut shortcut_parse(const std::wstring& text) {
    Shortcut result;unsigned value=0;std::size_t count=0;bool digits=false;
    for(auto ch:text+L",") {
        if(ch>=L'0' && ch<=L'9'){value=std::min(999u,value*10+ch-L'0');digits=true;continue;}
        if(ch!=L'+' && ch!=L',' && ch!=L' ')return {};
        if(digits) {
            value=shortcut_key(value);
            if(value<8 || value>254 || count>=result.keys.size())return {};
            if(std::find(result.keys.begin(),result.keys.end(),value)==result.keys.end())result.keys[count++]=static_cast<std::uint8_t>(value);
        }
        value=0;digits=false;
    }
    std::sort(result.keys.begin(),result.keys.begin()+count);
    return result;
}
inline std::wstring shortcut_string(const Shortcut& shortcut) {
    std::wstring text;
    for(auto key:shortcut.keys)if(key){if(!text.empty())text+=L"+";text+=std::to_wstring(key);}
    return text;
}
template<std::size_t N> class ShortcutMatcher {
public:
    void reset() { down_={};active_={}; }
    template<class Trigger> void input(unsigned key,bool down,const std::array<Shortcut,N>& bindings,Trigger trigger) {
        key=shortcut_key(key);if(key>=down_.size() || !key || down_[key]==down)return;
        down_[key]=down;
        for(std::size_t i=0;i<N;++i) {
            bool match=!bindings[i].empty();
            for(unsigned k=1;k<down_.size();++k) {
                const bool expected=std::find(bindings[i].keys.begin(),bindings[i].keys.end(),k)!=bindings[i].keys.end();
                if(down_[k]!=expected){match=false;break;}
            }
            if(match && !active_[i] && down)trigger(i);
            active_[i]=match;
        }
    }
private:
    std::array<bool,256> down_{};
    std::array<bool,N> active_{};
};
} // namespace arkhelp
