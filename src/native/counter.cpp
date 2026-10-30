#include "counter.h"
#include <charconv>
#include <string>
#include <cstdint>
#include <cstring>
namespace {
std::string_view tagName(std::string_view xml) {
    auto end=xml.find_first_of(" />",1);
    if(xml.empty()||xml.front()!='<'||end==xml.npos)return {};
    return xml.substr(1,end-1);
}
std::string_view attr(std::string_view s, std::string_view key) {
    std::string needle = " " + std::string(key) + "=\"";
    auto p = s.find(needle);
    if (p == s.npos) return {};
    s.remove_prefix(p + needle.size());
    auto end = s.find('"');
    return end == s.npos ? std::string_view{} : s.substr(0, end);
}
bool number(std::string_view s, int& n) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data()+s.size(), n);
    return r.ec == std::errc{} && r.ptr == s.data()+s.size();
}
}
bool IsRoundInitialization(std::string_view xml) {
    auto tag=tagName(xml);
    return tag=="INIT"||tag=="REINIT";
}
int RemainingFromNativeWall(std::string_view state) {
    // SHA-locked 130 client: 0x498450 tests exhaustion with
    // 136 - wall cursor - kans - four byte-vector lengths <= 14.
    // REINIT (0x4937d6..0x4944db) rebuilds those same fields on every seek.
    if(state.size()<0x1de)return -1;
    auto word=[&](size_t offset) {
        std::uint32_t value;std::memcpy(&value,state.data()+offset,sizeof(value));return value;
    };
    const unsigned separate=static_cast<unsigned char>(state[0x1dd]);
    if(separate>1)return -1;
    unsigned used=0;
    for(unsigned seat=0;seat<(separate?4u:1u);++seat) {
        auto cursor=word(0x1cc+seat*4);
        if(cursor>136)return -1;
        used+=cursor;
    }
    auto kans=word(0xc8);
    if(kans>4)return -1;
    used+=kans;
    unsigned nuki=0;
    for(unsigned seat=0;seat<4;++seat) {
        auto begin=word(0x8c+seat*0x10),end=word(0x90+seat*0x10);
        // These are byte vectors; inspect lengths only, never dereference them.
        if(end<begin||end-begin>4||(!begin&&end))return -1;
        nuki+=end-begin;
    }
    if(nuki>4)return -1;
    used+=nuki;
    const bool sanma=(word(0x100)&0x10)!=0;
    if(used>122||used<(sanma?67u:52u))return -1;
    return 122-static_cast<int>(used);
}
void WallCounter::restore(int remaining) {
    clear();
    if(remaining>=0&&remaining<=(sanma_?55:70)) {
        remaining_=remaining;active_=true;
    }
}
bool BeginsRoundLoading(std::string_view xml) {
    auto tag=tagName(xml);
    return tag=="GO"||tag=="TAIKYOKU"||tag=="SAIKAI"||tag=="INIT"||tag=="REINIT"||
        (tag=="UN"&&xml.find(" n0=\"")!=xml.npos);
}
void WallCounter::event(std::string_view xml) {
    if (xml.size()<3 || xml.front()!='<') return;
    auto e = xml.find_first_of(" />", 1);
    if (e==xml.npos) return;
    auto tag = xml.substr(1,e-1);
    if (tag=="GO") { int t; if (number(attr(xml,"type"),t)) set_type(t); clear(); }
    else if (tag=="TAIKYOKU") clear(); // A new CPU game can start without GO/BYE.
    else if (tag=="INIT") { remaining_=sanma_?55:70; active_=true; }
    else if (tag=="REINIT") {
        // XML alone is not a draw history. The hook restores the count from
        // native wall geometry AFTER the client has finished applying REINIT.
        clear();
    } else if (tag=="AGARI" || tag=="RYUUKYOKU" || tag=="BYE" || tag=="PROF") clear();
    else if (!tag.empty() && (tag[0]=='T'||tag[0]=='U'||tag[0]=='V'||tag[0]=='W'||
             tag[0]=='t'||tag[0]=='u'||tag[0]=='v'||tag[0]=='w')) {
        bool draw = tag.size()==1;
        if (!draw) { int tile; draw=number(tag.substr(1),tile)&&tile>=0&&tile<136; }
        // Each replacement draw consumes one live-wall position as well.
        // N (kan/nuki), DORA and REACH themselves must not also decrement.
        if (draw && active_) { if (remaining_>0) --remaining_; else clear(); }
    }
}
