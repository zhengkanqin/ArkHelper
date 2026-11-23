#include "../src/config/shortcuts.h"
#include <cstdio>
#include <stdexcept>
int main() {
    using namespace arkhelp;
    try {
        auto check=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
        std::array<Shortcut,4> bindings{shortcut_parse(L"65"),shortcut_parse(L"17+65"),shortcut_parse(L"81+69"),shortcut_parse(L"16+17+80")};
        check(shortcut_string(bindings[1])==L"17+65" && bindings[2]==shortcut_parse(L"69+81"),"chord order changed identity");
        check(shortcut_parse(L"162+65")==bindings[1] && shortcut_parse(L"999").empty() && shortcut_parse(L"65x").empty(),"normalization/invalid key handling failed");
        ShortcutMatcher<4> matcher;std::array<int,4> hits{};
        auto key=[&](unsigned k,bool down){matcher.input(k,down,bindings,[&](std::size_t i){++hits[i];});};
        key(65,true);key(65,true);key(65,true);check(hits[0]==1,"held single key repeated");
        key(65,false);key(65,true);check(hits[0]==2,"released single key did not rearm");
        matcher.reset();key(0xa2,true);key(65,true);key(65,true);
        check(hits[0]==2 && hits[1]==1,"combo triggered single key or repeated");
        key(65,false);key(65,true);check(hits[1]==2,"combo did not rearm while modifier held");
        matcher.reset();key(69,true);key(81,true);key(81,true);check(hits[2]==1,"two ordinary key chord failed");
        matcher.reset();key(17,true);key(16,true);key(80,true);check(hits[3]==1,"multiple modifier chord failed");
        matcher.reset();key(80,true);check(hits[3]==1,"focus reset retained stuck modifiers");
        std::puts("PASS shortcuts: single/chords, physical modifiers, repeat suppression, release rearm and focus reset");
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
