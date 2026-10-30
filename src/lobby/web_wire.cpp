#include "web_wire.h"
#include "lobby_protocol.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <vector>

namespace WebWire {
namespace {
[[noreturn]] void invalid(){throw std::runtime_error("unsupported web protocol message");}
struct Value {
    enum Kind {Scalar,Null,Object,Array} kind=Scalar;
    std::string text;
    std::map<std::string,Value> object;
    std::vector<Value> array;
};
class Parser {
    std::string_view input;size_t at=0;
    void space(){while(at<input.size()&&(input[at]==' '||input[at]=='\n'||input[at]=='\r'||input[at]=='\t'))++at;}
    char take(){if(at==input.size())invalid();return input[at++];}
    bool eat(char c){space();if(at<input.size()&&input[at]==c){++at;return true;}return false;}
    unsigned hex(){unsigned n=0;for(int i=0;i<4;++i){char c=take();unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;if(d==16)invalid();n=n*16+d;}return n;}
    void utf8(std::string& out,unsigned c){
        if(c==0||c>0x10ffff||(c>=0xd800&&c<=0xdfff))invalid();
        if(c<0x80)out+=static_cast<char>(c);
        else if(c<0x800){out+=static_cast<char>(0xc0|(c>>6));out+=static_cast<char>(0x80|(c&63));}
        else if(c<0x10000){out+=static_cast<char>(0xe0|(c>>12));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}
        else {out+=static_cast<char>(0xf0|(c>>18));out+=static_cast<char>(0x80|((c>>12)&63));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}
    }
    std::string string(){
        if(take()!='"')invalid();std::string out;
        for(;;){char c=take();if(c=='"')return out;if(static_cast<unsigned char>(c)<32)invalid();
            if(c!='\\'){out+=c;continue;}
            switch(take()){
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;case 'b':out+='\b';break;case 'f':out+='\f';break;
            case 'u':{unsigned u=hex();if(u>=0xd800&&u<=0xdbff){if(take()!='\\'||take()!='u')invalid();unsigned v=hex();if(v<0xdc00||v>0xdfff)invalid();u=0x10000+((u-0xd800)<<10)+v-0xdc00;}utf8(out,u);break;}
            default:invalid();}
        }
    }
    Value value(unsigned depth){
        if(depth>16)invalid();space();if(at==input.size())invalid();Value v;
        if(input[at]=='"'){v.text=string();return v;}
        if(eat('{')){v.kind=Value::Object;if(eat('}'))return v;do{space();auto key=string();if(!eat(':'))invalid();if(!v.object.emplace(key,value(depth+1)).second)invalid();}while(eat(','));if(!eat('}'))invalid();return v;}
        if(eat('[')){v.kind=Value::Array;if(eat(']'))return v;do{v.array.push_back(value(depth+1));}while(eat(','));if(!eat(']'))invalid();return v;}
        auto start=at;while(at<input.size()&&input[at]!=','&&input[at]!=']'&&input[at]!='}'&&!std::isspace(static_cast<unsigned char>(input[at])))++at;
        auto token=input.substr(start,at-start);
        if(token=="null"){v.kind=Value::Null;return v;}if(token=="true"){v.text="1";return v;}if(token=="false"){v.text="0";return v;}
        // Wire numbers are preserved without floating point rounding.
        size_t p=0;if(p<token.size()&&token[p]=='-')++p;
        if(p==token.size())invalid();
        if(token[p]=='0')++p;else{if(token[p]<'1'||token[p]>'9')invalid();while(p<token.size()&&std::isdigit(static_cast<unsigned char>(token[p])))++p;}
        if(p<token.size()&&token[p]=='.'){size_t begin=++p;while(p<token.size()&&std::isdigit(static_cast<unsigned char>(token[p])))++p;if(p==begin)invalid();}
        if(p<token.size()&&(token[p]=='e'||token[p]=='E')){++p;if(p<token.size()&&(token[p]=='+'||token[p]=='-'))++p;size_t begin=p;while(p<token.size()&&std::isdigit(static_cast<unsigned char>(token[p])))++p;if(p==begin)invalid();}
        if(p!=token.size())invalid();v.text=std::string(token);return v;
    }
public:
    explicit Parser(std::string_view s):input(s){}
    Value parse(){if(input.size()>1024*1024)invalid();auto v=value(0);space();if(at!=input.size())invalid();return v;}
};
bool name(std::string_view s){return !s.empty()&&s.size()<=32&&(std::isalpha(static_cast<unsigned char>(s[0]))||s[0]=='_')&&std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isalnum(c)||c=='_';});}
std::string scalar(const Value& v){
    if(v.kind==Value::Scalar)return v.text;if(v.kind==Value::Null)return {};
    if(v.kind!=Value::Array)invalid();std::string out;
    for(size_t i=0;i<v.array.size();++i){if(i)out+=',';if(v.array[i].kind!=Value::Scalar&&v.array[i].kind!=Value::Null)invalid();out+=v.array[i].text;}return out;
}
std::string escape(std::string_view s){
    std::string out;for(unsigned char c:s){switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;default:if(c==0||(c<32&&c!='\t'&&c!='\r'&&c!='\n'))invalid();out+=static_cast<char>(c);}}return out;
}
std::string counts(std::string_view csv){
    // Inverse of the native LN base-52 decoder. Consecutive empty CSV
    // entries are represented by the separator run between numeric words.
    std::string out;size_t at=0;unsigned separators=0;
    for(;;){auto end=csv.find(',',at);auto word=csv.substr(at,end==csv.npos?csv.size()-at:end-at);
        if(!word.empty()){
            int n=0;if(!LobbyProtocol::Integer(word,-10000000,10000000,n))invalid();
            if(separators){out+=std::to_string(separators);separators=0;}if(n<0){out+='-';n=-n;}
            std::string digits;do{unsigned digit=n%52;digits+=static_cast<char>(digit<26?'A'+digit:'a'+digit-26);n/=52;}while(n);
            std::reverse(digits.begin(),digits.end());out+=digits;
        }
        if(end==csv.npos){if(separators)out+=std::to_string(separators);break;}++separators;at=end+1;
    }return out;
}
std::string event(const Value& v,unsigned depth=0){
    if(depth>16||v.kind!=Value::Object)invalid();auto ti=v.object.find("tag");if(ti==v.object.end())invalid();auto tag=scalar(ti->second);if(!name(tag))invalid();
    bool tile=tag.size()==1&&std::string_view("TUVWtuvwDEFGdefg").find(tag[0])!=std::string_view::npos;
    std::string tileKey;
    if(tile){for(auto key:{"0","p"}){auto p=v.object.find(key);if(p!=v.object.end()){int n=0;if(!LobbyProtocol::Integer(scalar(p->second),0,136,n))invalid();if(n<136)tag+=std::to_string(n);tileKey=key;break;}}}
    std::string out="<"+tag;
    for(const auto& item:v.object){
        const auto& key=item.first;if(key=="tag"||key=="childNodes"||key==tileKey||item.second.kind==Value::Null)continue;
        if(!name(key))invalid();auto text=scalar(item.second);
        if(tag=="LN"&&(key=="n"||key=="j"||key=="g"))text=counts(text);
        out+=" "+key+"=\""+escape(text)+"\"";
    }
    auto children=v.object.find("childNodes");
    if(children==v.object.end())return out+" />";
    if(children->second.kind!=Value::Array)invalid();out+='>';
    for(const auto& child:children->second.array){if(child.kind==Value::Scalar)out+=escape(child.text);else out+=event(child,depth+1);}
    return out+"</"+tag+">";
}
std::string attribute(std::string_view xml,std::string_view key){
    auto needle=" "+std::string(key)+"=\"";auto at=xml.find(needle);if(at==xml.npos)return {};at+=needle.size();auto end=xml.find('"',at);if(end==xml.npos)invalid();return std::string(xml.substr(at,end-at));
}
}
std::string ToNative(std::string_view json){
    auto v=Parser(json).parse();std::string out;
    if(v.kind==Value::Array){for(const auto& item:v.array){out+=event(item);out+='\0';}}
    else {out=event(v);out+='\0';}
    if(out.size()>2*1024*1024)invalid();return out;
}
std::string ToWeb(std::string_view xml){
    if(xml.empty()||xml.size()>65536||xml.find('\0')!=xml.npos)invalid();
    auto tag=LobbyProtocol::Tag(xml);if(tag.empty())invalid();
    // The official /3/ HELO contains name, sx and optional gpid. In
    // particular it does not contain the native client's tid="w0" marker.
    if(tag=="HELO"){
        auto id=attribute(xml,"name"),sex=attribute(xml,"sx"),resume=attribute(xml,"gpid");
        if(id.empty())id="NoName";if(sex!="F")sex="M";
        std::string out="{\"tag\":\"HELO\",\"name\":"+LobbyProtocol::Quote(id)+",\"sx\":"+LobbyProtocol::Quote(sex);
        if(!resume.empty())out+=",\"gpid\":"+LobbyProtocol::Quote(resume);return out+"}";
    }
    auto integer=[&](const char* key,int max){
        int n=0;if(!LobbyProtocol::Integer(attribute(xml,key),0,max,n))invalid();return std::to_string(n);
    };
    if(tag=="REACH"){
        // Windows includes a tenpai candidate as a validation hint, BEFORE
        // the user chooses a discard, and locally applies REACH step=1.
        // Declare only. The original table sends D after the actual click;
        // sending that candidate now advances the server behind the table UI.
        const auto tile=attribute(xml,"hai");
        if(!tile.empty())integer("hai",135);
        return "{\"tag\":\"REACH\"}";
    }
    if(tag=="D")return "{\"tag\":\"D\",\"p\":"+integer("p",135)+"}";
    if(tag=="N"){
        std::string out="{\"tag\":\"N\"";
        if(!attribute(xml,"type").empty())out+=",\"type\":"+integer("type",10);
        for(auto key:{"hai","hai0","hai1"})if(!attribute(xml,key).empty())out+=",\""+std::string(key)+"\":"+integer(key,135);
        return out+"}";
    }
    if(tag=="JOIN"){
        const auto reservation=attribute(xml,"t");
        return reservation.empty()?"{\"tag\":\"JOIN\"}":"{\"tag\":\"JOIN\",\"t\":"+LobbyProtocol::Quote(reservation)+"}";
    }
    if(tag=="GOK"||tag=="NEXTREADY"||tag=="BYE")return "{\"tag\":"+LobbyProtocol::Quote(tag)+"}";
    // Heartbeat and PXR use XML in the official Web client as well. Other
    // existing lobby commands retain their established wire representation.
    return std::string(xml);
}
}
