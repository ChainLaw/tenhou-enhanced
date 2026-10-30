#include <winsock2.h>
#include "web_transport.h"
#include "lobby_connection.h"
#include "web_wire.h"
#include "pe_imports.h"
#include <winhttp.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace WebTransport {
namespace {
std::atomic<bool> enabled{false};
std::atomic<bool> active{false};
BYTE* client=nullptr;
std::wstring logPath;
bool diagnostics=false;
std::mutex logMutex;
using Connect=int(WSAAPI*)(SOCKET,const sockaddr*,int);
using Send=int(WSAAPI*)(SOCKET,const char*,int,int);
using Recv=int(WSAAPI*)(SOCKET,char*,int,int);
using Close=int(WSAAPI*)(SOCKET);
using Select=int(WSAAPI*)(SOCKET,HWND,u_int,long);
Connect originalConnect=nullptr;Send originalSend=nullptr;Recv originalRecv=nullptr;
Close originalClose=nullptr;Select originalSelect=nullptr;
constexpr size_t maxQueue=2*1024*1024;
void log(const char* stage,DWORD code=0,bool failure=false){
    if(!diagnostics&&!failure)return;
    std::lock_guard<std::mutex> lock(logMutex);
    HANDLE f=CreateFileW(logPath.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;
    char line[192];int count=sprintf_s(line,"[%lu:%llu] Web3: %s (%lu)\r\n",GetCurrentProcessId(),GetTickCount64(),stage,code);DWORD written=0;
    if(count>0)WriteFile(f,line,count,&written,nullptr);CloseHandle(f);
}
struct Internet {
    HINTERNET h=nullptr;
    explicit Internet(HINTERNET handle):h(handle){}
    ~Internet(){if(h)WinHttpCloseHandle(h);}
    Internet(const Internet&)=delete;Internet& operator=(const Internet&)=delete;
};
struct Registration {HWND window=nullptr;UINT message=0;long events=0;};
struct Connection {
    SOCKET socket=INVALID_SOCKET;
    std::mutex state,io;
    std::condition_variable changed;
    Registration registration;
    std::atomic<bool> stopped{false};
    bool connected=false,closed=false,readPosted=false;
    int error=0;
    std::deque<std::string> outbound;
    size_t outboundBytes=0;
    std::string incoming,partial;
    size_t readOffset=0;
    HINTERNET webSocket=nullptr; // io guards send/close and handle lifetime.
    void notifyLocked(long event,int failure=0){
        if(!stopped&&registration.window&&(registration.events&event))
            PostMessageW(registration.window,registration.message,socket,WSAMAKESELECTREPLY(event,failure));
    }
    void readableLocked(){if(!readPosted&&readOffset<incoming.size()&&(registration.events&FD_READ)){readPosted=true;notifyLocked(FD_READ);}}
    void finish(bool wasConnected,int failure){
        std::lock_guard<std::mutex> lock(state);if(closed||stopped)return;closed=true;error=failure;
        // Keep a final queued game event ahead of FD_CLOSE.
        readableLocked();notifyLocked(wasConnected?FD_CLOSE:FD_CONNECT,failure);
    }
    void cancel(){
        stopped=true;changed.notify_all();
        std::lock_guard<std::mutex> lock(io);
        if(webSocket)WinHttpWebSocketClose(webSocket,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,nullptr,0);
    }
    // closesocket must not discard a just-queued logout (BYE): the server
    // otherwise keeps the account connected and rejects the next HELO.
    void flush(){
        std::deque<std::string> pending;
        {std::lock_guard<std::mutex> lock(state);if(stopped||closed)return;pending.swap(outbound);outboundBytes=0;}
        std::lock_guard<std::mutex> lock(io);
        for(auto& packet:pending){
            if(webSocket)WinHttpWebSocketSend(webSocket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,packet.data(),static_cast<DWORD>(packet.size()));
            std::fill(packet.begin(),packet.end(),'\0');
        }
    }
};
std::mutex socketsMutex;
std::unordered_map<SOCKET,Registration> registrations;
std::unordered_map<SOCKET,std::shared_ptr<Connection>> sockets;
std::shared_ptr<Connection> find(SOCKET s){std::lock_guard<std::mutex> lock(socketsMutex);auto it=sockets.find(s);return it==sockets.end()?nullptr:it->second;}
int fail(int code){WSASetLastError(code);return SOCKET_ERROR;}

void sender(const std::shared_ptr<Connection>& c){
    for(;;){
        std::string packet;
        {std::unique_lock<std::mutex> lock(c->state);c->changed.wait(lock,[&]{return c->stopped||!c->outbound.empty();});if(c->stopped)return;
            packet=std::move(c->outbound.front());c->outbound.pop_front();c->outboundBytes-=packet.size();}
        DWORD error=ERROR_OPERATION_ABORTED;
        {std::lock_guard<std::mutex> lock(c->io);if(!c->stopped&&c->webSocket)error=WinHttpWebSocketSend(c->webSocket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,packet.data(),static_cast<DWORD>(packet.size()));}
        std::fill(packet.begin(),packet.end(),'\0');
        if(error){if(!c->stopped){log("send failed",error,true);c->finish(true,WSAECONNRESET);c->cancel();}return;}
    }
}
void run(const std::shared_ptr<Connection>& c){
    // No certificate-validation exceptions and no silent TCP fallback.
    Internet session(WinHttpOpen(L"TenhouEnhanced Web3",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    if(!session.h){log("session failed",GetLastError());c->finish(false,WSAENETUNREACH);return;}
    WinHttpSetTimeouts(session.h,10000,10000,10000,30000);
    Internet host(WinHttpConnect(session.h,L"b-ww.mjv.jp",INTERNET_DEFAULT_HTTPS_PORT,0));
    Internet request(host.h?WinHttpOpenRequest(host.h,L"GET",L"/",nullptr,L"https://tenhou.net/3/",WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr);
    DWORD disable=WINHTTP_DISABLE_REDIRECTS;
    if(!request.h||!WinHttpSetOption(request.h,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable))||
       !WinHttpSetOption(request.h,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0)||
       !WinHttpSendRequest(request.h,L"Origin: https://tenhou.net\r\n",static_cast<DWORD>(-1),nullptr,0,0,0)||
       !WinHttpReceiveResponse(request.h,nullptr)){
        log("WebSocket handshake failed",GetLastError());c->finish(false,WSAECONNREFUSED);return;
    }
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr)||status!=101){log("WebSocket HTTP status",status);c->finish(false,WSAECONNREFUSED);return;}
    Internet ws(WinHttpWebSocketCompleteUpgrade(request.h,0));
    if(!ws.h){log("upgrade failed",GetLastError());c->finish(false,WSAECONNREFUSED);return;}
    DWORD closeTimeout=1000;WinHttpSetOption(ws.h,WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,&closeTimeout,sizeof(closeTimeout));
    {std::lock_guard<std::mutex> lock(c->io);if(c->stopped)return;c->webSocket=ws.h;}
    log("connected to b-ww.mjv.jp; awaiting server login");
    {std::lock_guard<std::mutex> lock(c->state);c->connected=true;c->notifyLocked(FD_CONNECT);}
    std::thread sendThread;
    try{sendThread=std::thread(sender,c);}catch(...){c->finish(true,WSAENOBUFS);c->cancel();std::lock_guard<std::mutex> lock(c->io);c->webSocket=nullptr;return;}
    int failure=0;
    try{
        std::string message;char buffer[16384];
        while(!c->stopped){
            DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE kind{};
            DWORD result=WinHttpWebSocketReceive(ws.h,buffer,sizeof(buffer),&count,&kind);
            if(result){if(!c->stopped){log("receive failed",result,true);failure=WSAECONNRESET;}break;}
            if(kind==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE){
                USHORT closeStatus=0;char reason[123]{};DWORD length=0;
                WinHttpWebSocketQueryCloseStatus(ws.h,&closeStatus,reason,sizeof(reason),&length);
                log("server closed connection",closeStatus,true);break;
            }
            if(kind!=WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE&&kind!=WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)throw std::runtime_error("binary response");
            if(message.size()+count>1024*1024)throw std::runtime_error("oversized response");
            message.append(buffer,count);if(kind==WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)continue;
            auto xml=WebWire::ToNative(message);std::fill(message.begin(),message.end(),'\0');message.clear();
            std::lock_guard<std::mutex> lock(c->state);
            if(c->readOffset){c->incoming.erase(0,c->readOffset);c->readOffset=0;}
            if(c->incoming.size()+xml.size()>maxQueue)throw std::runtime_error("receive queue full");
            c->incoming+=xml;c->readableLocked();
        }
    }catch(...){log("unsupported or malformed server message; connection stopped",0,true);failure=WSAEPROTONOSUPPORT;}
    if(!c->stopped)c->finish(true,failure);
    c->cancel();if(sendThread.joinable())sendThread.join();
    {std::lock_guard<std::mutex> lock(c->io);c->webSocket=nullptr;}
}

int WSAAPI selectHook(SOCKET s,HWND window,u_int message,long events){
    auto c=find(s);
    if(c){std::lock_guard<std::mutex> lock(c->state);c->registration={window,message,events};c->readPosted=false;c->readableLocked();return 0;}
    const int result=originalSelect(s,window,message,events);
    if(!result){std::lock_guard<std::mutex> lock(socketsMutex);if(events)registrations[s]={window,message,events};else registrations.erase(s);}
    return result;
}
int WSAAPI connectHook(SOCKET s,const sockaddr* address,int length){
    // The supported binary's game connection calls connect at RVA 0x16410.
    // Resource downloads and other sockets retain their original APIs.
    if(!enabled||!active||_ReturnAddress()!=client+0x16415)return originalConnect(s,address,length);
    std::shared_ptr<Connection> c,retired;
    try{
        c=std::make_shared<Connection>();c->socket=s;
        {std::lock_guard<std::mutex> lock(socketsMutex);auto found=registrations.find(s);
            if(found==registrations.end()||found->second.message!=0x8201)return fail(WSAEINVAL);
            auto previous=sockets.find(s);
            if(previous!=sockets.end()){
                std::lock_guard<std::mutex> state(previous->second->state);
                if(!previous->second->closed&&!previous->second->stopped)return fail(WSAEALREADY);
                retired=previous->second;sockets.erase(previous);
            }
            c->registration=found->second;sockets.emplace(s,c);}
        if(retired)retired->cancel();
        originalSelect(s,c->registration.window,0,0);
        std::thread([c]{try{run(c);}catch(...){log("transport worker failed");c->finish(c->connected,WSAENOBUFS);}}).detach();
    }catch(...){std::lock_guard<std::mutex> lock(socketsMutex);sockets.erase(s);return fail(WSAENOBUFS);}
    return fail(WSAEWOULDBLOCK);
}
int WSAAPI sendHook(SOCKET s,const char* data,int length,int flags){
    auto c=find(s);if(!c)return originalSend(s,data,length,flags);
    if(length<0||(!data&&length))return fail(WSAEFAULT);if(flags)return fail(WSAEOPNOTSUPP);if(!length)return 0;
    try{
        std::lock_guard<std::mutex> lock(c->state);
        if(c->stopped||c->closed)return fail(WSAECONNRESET);if(!c->connected)return fail(WSAENOTCONN);
        if(c->partial.size()+static_cast<size_t>(length)>65536||c->outboundBytes+static_cast<size_t>(length)>maxQueue)return fail(WSAENOBUFS);
        auto pending=c->partial+std::string(data,length);size_t at=0;
        // Stage complete frames before committing, so a conversion failure
        // never leaves an ambiguous partially submitted send.
        std::vector<std::string> packets;size_t added=0;
        for(auto end=pending.find('\0');end!=pending.npos;end=pending.find('\0',at)){
            if(end>at){
                auto packet=WebWire::ToWeb(std::string_view(pending).substr(at,end-at));
                added+=packet.size();packets.push_back(std::move(packet));
            }
            at=end+1;
        }
        if(c->outboundBytes+added>maxQueue)return fail(WSAENOBUFS);
        for(auto& packet:packets)c->outbound.push_back(std::move(packet));c->outboundBytes+=added;c->partial=pending.substr(at);
        c->changed.notify_one();return length;
    }catch(...){log("unsupported native outgoing message",0,true);return fail(WSAEINVAL);}
}
int WSAAPI recvHook(SOCKET s,char* data,int length,int flags){
    auto c=find(s);if(!c)return originalRecv(s,data,length,flags);
    if(length<0||(!data&&length))return fail(WSAEFAULT);if(flags&~MSG_PEEK)return fail(WSAEOPNOTSUPP);if(!length)return 0;
    std::lock_guard<std::mutex> lock(c->state);c->readPosted=false;
    const size_t available=c->incoming.size()-c->readOffset;
    if(!available){if(c->closed)return c->error?fail(c->error):0;return fail(WSAEWOULDBLOCK);}
    const int count=static_cast<int>(std::min(available,static_cast<size_t>(length)));memcpy(data,c->incoming.data()+c->readOffset,count);
    if(!(flags&MSG_PEEK))c->readOffset+=count;c->readableLocked();return count;
}
int WSAAPI closeHook(SOCKET s){
    std::shared_ptr<Connection> c;
    {std::lock_guard<std::mutex> lock(socketsMutex);auto found=sockets.find(s);if(found!=sockets.end()){c=found->second;sockets.erase(found);}registrations.erase(s);}
    if(c){c->flush();c->cancel();}return originalClose(s);
}
bool fallbackToNative(){
    if(!enabled)return false;
    enabled=false;
    // Existing bridge sockets keep their send/recv/close handlers until the
    // native CONNECT path retires them. New connections use native Winsock.
    LobbyConnection::UseBackend(nullptr,ReleaseWindow);
    log("Web lobby unavailable; rebuilding the original Windows connection");
    return true;
}
}
bool Enabled(){return enabled;}
bool Active(){return enabled&&active;}
void SetActive(bool useWeb){
    if(active.exchange(useWeb)!=useWeb&&enabled)
        log(useWeb?"routing matchmaking to Web connection":"routing lobby/viewer to original Windows connection");
}
bool Connected(){
    if(!enabled||!client)return false;
    const auto socket=*reinterpret_cast<const DWORD*>(client+0xf4e3c);
    if(socket==MAXDWORD)return false;
    auto c=find(socket);
    if(!active)return !c;
    if(!c)return false;
    std::lock_guard<std::mutex> lock(c->state);return c->connected&&!c->closed&&!c->stopped;
}
void ReleaseWindow(HWND window){
    std::vector<std::shared_ptr<Connection>> retired;
    {std::lock_guard<std::mutex> lock(socketsMutex);
        for(auto it=sockets.begin();it!=sockets.end();){
            std::lock_guard<std::mutex> state(it->second->state);
            if(it->second->registration.window==window){registrations.erase(it->first);retired.push_back(it->second);it=sockets.erase(it);}else ++it;
        }
    }
    // Native FD_CLOSE clears its global socket before destroying WC_CLIENT,
    // so its destructor cannot close that handle. Retire any remaining bridge
    // connection here, including an interrupted asynchronous handshake.
    for(auto& c:retired){c->cancel();originalClose(c->socket);}
}
bool Install(HMODULE executable,const std::wstring& root){
    const auto ini=root+L"\\enhanced.ini";
    if(!GetPrivateProfileIntW(L"network",L"web3",0,ini.c_str()))return true;
    diagnostics=GetPrivateProfileIntW(L"diagnostics",L"events",0,ini.c_str())!=0;
    logPath=root+L"\\enhanced.log";client=reinterpret_cast<BYTE*>(executable);
    const BYTE call[]={0xe8,0x0f,0x79,0x0c,0x00};
    if(memcmp(client+0x16410,call,sizeof(call))){log("native connect signature mismatch");return false;}
    struct Hook {unsigned ordinal;const char* name;void* target;DWORD* slot=nullptr;DWORD original=0;};
    Hook hooks[]={{4,"connect",reinterpret_cast<void*>(connectHook)},{19,"send",reinterpret_cast<void*>(sendHook)},
        {16,"recv",reinterpret_cast<void*>(recvHook)},{3,"closesocket",reinterpret_cast<void*>(closeHook)},
        {101,"WSAAsyncSelect",reinterpret_cast<void*>(selectHook)}};
    PeImports::Visit(executable,[&](const PeImports::Import& entry){
        if(_stricmp(entry.library,"wsock32.dll"))return true;
        for(auto& hook:hooks)if((entry.name&&!strcmp(entry.name,hook.name))||(!entry.name&&entry.ordinal==hook.ordinal)){hook.slot=entry.slot;hook.original=*entry.slot;}
        return true;
    });
    for(const auto& hook:hooks)if(!hook.slot){log("required socket import missing");return false;}
    originalConnect=reinterpret_cast<Connect>(hooks[0].original);originalSend=reinterpret_cast<Send>(hooks[1].original);
    originalRecv=reinterpret_cast<Recv>(hooks[2].original);originalClose=reinterpret_cast<Close>(hooks[3].original);originalSelect=reinterpret_cast<Select>(hooks[4].original);
    for(size_t i=0;i<5;++i)if(!PeImports::Replace(hooks[i].slot,hooks[i].target)){
        while(i){--i;PeImports::Replace(hooks[i].slot,reinterpret_cast<void*>(hooks[i].original));}log("socket import installation failed");return false;
    }
    // Startup resumes use Web only when the native client retained a GPID.
    // Ordinary logins start on the original Windows connection.
    wchar_t resume[257]{};GetPrivateProfileStringW(L"STATUS",L"GPID",L"",resume,257,(root+L"\\config.ini").c_str());
    active=*resume!=0;
    SecureZeroMemory(resume,sizeof(resume));
    enabled=true;LobbyConnection::UseBackend(Connected,ReleaseWindow,fallbackToNative);log("/3/ matchmaking transport enabled");return true;
}
}
