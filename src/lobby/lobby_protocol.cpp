#include "lobby_protocol.h"
#include <windows.h>
#include <charconv>
#include <algorithm>
#include <cctype>
namespace LobbyProtocol {
std::string UrlEncode(std::string_view value) {
    const char hex[]="0123456789ABCDEF";
    std::string out;out.reserve(value.size()*3);
    for(unsigned char c:value){out+='%';out+=hex[c>>4];out+=hex[c&15];}
    return out;
}
std::string DecodeCounts(std::string_view value) {
    // The Windows LN wire format (004C6630) uses base-52 letters for
    // nonzero integers and decimal runs for comma separators. /4 receives
    // expanded CSV from its JSON transport instead.
    if(value.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")==value.npos)return std::string(value);
    std::string out;unsigned number=0,separators=0;bool negative=false,hasNumber=false;
    auto flush=[&](){if(hasNumber){out+=(negative?"-":"")+std::to_string(number);number=0;}hasNumber=false;negative=false;};
    for(char c:value){
        if(c=='-'){negative=true;continue;}
        int digit=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:-1;
        if(digit>=0){if(separators>512||number>10000000/52)return {};out.append(separators,',');separators=0;number=number*52+digit;hasNumber=true;}
        else if(c>='0'&&c<='9'){flush();separators=separators*10+c-'0';if(separators>512)return {};}
        else return {};
    }
    flush();out.append(separators,',');
    return out.size()<=8192?out:std::string();
}
std::string Quote(std::string_view value) {
    std::string out="\"";const char hex[]="0123456789abcdef";
    for(unsigned char c:value){switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\r':out+="\\r";break;case '\n':out+="\\n";break;case '\t':out+="\\t";break;default:if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=static_cast<char>(c);}}
    return out+'"';
}
std::string Tag(std::string_view xml) {
    if(xml.size()<3||xml.front()!='<')return {};
    size_t end=1;while(end<xml.size()&&(std::isalnum(static_cast<unsigned char>(xml[end]))||xml[end]=='_'))++end;
    if(end==1||end>24||end==xml.size())return {};return std::string(xml.substr(1,end-1));
}
std::string FilterEvent(std::string_view xml) {
    if(xml.size()>1024*1024)return {};
    const auto tag=Tag(xml);
    if(tag=="LN"){
        std::string out="<LN";
        for(auto name:{"n","j","g"}){auto key=" "+std::string(name)+"=\"";auto at=xml.find(key);if(at==xml.npos)return {};at+=key.size();auto end=xml.find('"',at);if(end==xml.npos)return {};auto decoded=DecodeCounts(xml.substr(at,end-at));if(decoded.empty())return {};out+=" "+std::string(name)+"=\""+decoded+"\"";}
        return out+" />";
    }
    if(tag=="RANKING"||tag=="CS"||tag=="DATE"||tag=="CHAT")return std::string(xml);
    if(tag!="HELO"&&tag!="PROF")return {};
    // Authentication challenge, account ID, tile values and game events never
    // enter the browser. Initial profile attributes alone are sufficient.
    std::string out="<"+tag;size_t at=1+tag.size();bool profile=false;
    while(at<xml.size()) {
        while(at<xml.size()&&xml[at]==' ')++at;size_t start=at;
        while(at<xml.size()&&std::isalnum(static_cast<unsigned char>(xml[at])))++at;
        if(at==start||at+1>=xml.size()||xml[at]!='='||xml[at+1]!='"')break;
        auto name=xml.substr(start,at-start);at+=2;auto end=xml.find('"',at);if(end==xml.npos)break;
        bool pf=name.size()>=3&&name.substr(0,2)=="PF"&&std::all_of(name.begin()+2,name.end(),[](char c){return (c>='0'&&c<='9')||c=='C';});
        if(pf||name=="uname"||name=="expire"||name=="expiredays"||name=="opt") {out+=' ';out.append(name);out+="=\"";out.append(xml.substr(at,end-at));out+='"';profile=true;}
        at=end+1;
    }
    return profile?out+" />":std::string();
}
bool Integer(std::string_view value,int minimum,int maximum,int& result) {
    if(value.empty()||value.size()>10)return false;int n=0;auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
    if(parsed.ec!=std::errc()||parsed.ptr!=value.data()+value.size()||n<minimum||n>maximum)return false;result=n;return true;
}
int ReservationIndex(int type){const int rules[]={7,15,1,9,65,73,4161,4169,17,25,81,89};int base=type&~(32|128);for(int i=0;i<12;++i)if(base==rules[i])return i;return -1;}
unsigned NormalizeWatchFlags(unsigned flags){
    unsigned players=flags&3,winds=flags&12,tables=flags&112;
    if(!players)players=3;
    if(!winds)winds=12;
    // /4's lookup never terminates for an unknown group value. In particular,
    // native/private-room flags may omit the public-room table group entirely.
    if(tables!=16&&tables!=32&&tables!=64&&tables!=112)tables=64;
    return players|winds|tables;
}
std::string Utf8(const std::wstring& value){if(value.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),n,nullptr,nullptr);return out;}
std::wstring Wide(std::string_view value){if(value.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);std::wstring out(n,L'\0');if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n);return out;}
}
