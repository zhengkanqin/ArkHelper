#pragma once
#include <windows.h>
#include "../config/shortcuts.h"
namespace arkhelp {
inline std::wstring shortcut_label(const Shortcut& shortcut) {
    if(shortcut.empty())return L"未设置";
    std::wstring result;
    const auto append=[&](const std::wstring& text){if(!result.empty())result+=L" + ";result+=text;};
    for(auto key:{0x11,0x12,0x10,0x5b})if(std::find(shortcut.keys.begin(),shortcut.keys.end(),key)!=shortcut.keys.end())
        append(key==0x11?L"Ctrl":key==0x12?L"Alt":key==0x10?L"Shift":L"Win");
    for(auto key:shortcut.keys) {
        if(!key || key==0x10 || key==0x11 || key==0x12 || key==0x5b)continue;
        wchar_t text[64]{};UINT scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC);
        if((key>=VK_PRIOR && key<=VK_DELETE) || key==VK_DIVIDE || key==VK_NUMLOCK)scan|=0x100;
        if(GetKeyNameTextW(static_cast<LONG>(scan<<16),text,64))append(text);
        else append(L"Key "+std::to_wstring(key));
    }
    return result;
}
}
