#include "pe_imports.h"
#include "web_lobby.h"
#include "lobby_protocol.h"
#include "lobby_services.h"
#include "local_replays.h"
#include "lobby_connection.h"
#include "web_transport.h"
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <deque>
#include <regex>
#include <future>
#include <chrono>
#include <set>

namespace WebLobby {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using CreateA=HWND(WINAPI*)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
using SendXml=int(__cdecl*)(const char*);
CreateA previousCreate=nullptr;SendXml nativeSend=nullptr;
int __cdecl sendGameReturn(const char* xml);
int __cdecl leaveGame(const char* xml);
BOOL WINAPI postResultReturn(HWND w,UINT msg,WPARAM wp,LPARAM lp);
BOOL WINAPI postGameConnectionClose(HWND w,UINT msg,WPARAM wp,LPARAM lp);
BOOL WINAPI postDisconnectedLogin(HWND w,UINT msg,WPARAM wp,LPARAM lp);
LRESULT CALLBACK coverObserver(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
void attachCertificate(HWND);
void* nativeProfileEnd=nullptr;
thread_local bool deferNativeProfile=false;
bool __cdecl profileUiDeferred(){return deferNativeProfile;}
// Replace the five-byte mov/test immediately after native HELO/AUTH cleanup.
// Authentication runs unchanged; only its window transition can be deferred.
__declspec(naked) void nativeProfileGate(){
    __asm {
        pushad
        call profileUiDeferred
        test al,al
        popad
        jnz deferred
        mov eax,[ebp+24h]
        test eax,eax
        ret
    deferred:
        add esp,4
        jmp dword ptr [nativeProfileEnd]
    }
}
const DWORD* nativeSocket=nullptr;
using CloseSocket=int(WINAPI*)(UINT_PTR);
DWORD* nativeCloseSocketSlot=nullptr;
struct PendingSocketClose {
    DWORD socket;
    bool closed=false;
    void close();
};
std::unordered_map<HWND,std::shared_ptr<PendingSocketClose>> pendingSocketCloses;
bool returnHooks=false,returnPrefetchHooks=false;
std::wstring directory;HMODULE loader=nullptr;
constexpr UINT initializeMessage=WM_APP+0x341,commandMessage=WM_APP+0x342,repaintNativeMessage=WM_APP+0x343,nativeOpenFinishedMessage=WM_APP+0x345,nativeCertificateChangedMessage=WM_APP+0x346,nativeCertificateReadyMessage=WM_APP+0x347,bridgeTimer=0x344b;
constexpr UINT_PTR viewSubclass=0x344b5642;
constexpr UINT disconnectedLoginMessage=WM_APP+0x348;
constexpr UINT retireNetworkMessage=WM_APP+0x34b,resumeJoinMessage=WM_APP+0x34a;
constexpr UINT prepareReturnMessage=WM_APP+0x34c,finishReturnMessage=WM_APP+0x34d;
constexpr UINT openNativeViewerMessage=WM_APP+0x34e;
constexpr UINT nativeSettingsCommand=0x1f10;
constexpr UINT defaultWatchFlags=3|12|64; // /4: － (四/三), － (東/南), 鳳
constexpr wchar_t origin[]=L"https://tenhou.local/";
void diagnostic(const char* value) {
    HANDLE file=CreateFileW((directory+L"\\enhanced.log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD n=0;WriteFile(file,value,static_cast<DWORD>(strlen(value)),&n,nullptr);WriteFile(file,"\r\n",2,&n,nullptr);CloseHandle(file);}
}
void returnTrace(const char* event,unsigned attempt=0){
    char line[208];sprintf_s(line,"Game return v2: pid=%lu tick=%llu socket=%lu attempt=%u %s",GetCurrentProcessId(),GetTickCount64(),nativeSocket?*nativeSocket:MAXDWORD,attempt,event);diagnostic(line);
}
void connectionTrace(const char* event,unsigned code=0){
    char line[224];sprintf_s(line,"Connection: pid=%lu tick=%llu socket=%lu code=%u %s",GetCurrentProcessId(),GetTickCount64(),nativeSocket?*nativeSocket:MAXDWORD,code,event);diagnostic(line);
}
void PendingSocketClose::close(){
    if(closed||!nativeCloseSocketSlot||(nativeSocket&&*nativeSocket==socket))return;
    closed=true;
    const auto result=reinterpret_cast<CloseSocket>(*nativeCloseSocketSlot)(socket);
    connectionTrace(result==0?"retired FD_CLOSE socket released":"retired FD_CLOSE socket release failed");
}
bool classIs(HWND w,const wchar_t* value){wchar_t cls[80]{};return w&&GetClassNameW(w,cls,80)&&!wcscmp(cls,value);}
bool visible(HWND w){return w&&IsWindow(w)&&IsWindowVisible(w);}
HBITMAP captureWindowFrame(HWND window,SIZE& size){
    if(!visible(window)||IsIconic(GetAncestor(window,GA_ROOT)))return nullptr;
    RECT r{};if(!GetClientRect(window,&r)||r.right<=0||r.bottom<=0)return nullptr;
    // Include native child controls without inheriting WS_CLIPCHILDREN.
    auto source=GetDCEx(window,nullptr,DCX_CACHE);if(!source)return nullptr;
    auto memory=CreateCompatibleDC(source);
    auto bitmap=CreateCompatibleBitmap(source,r.right,r.bottom);
    HGDIOBJ previous=memory&&bitmap?SelectObject(memory,bitmap):nullptr;
    const bool copied=previous&&BitBlt(memory,0,0,r.right,r.bottom,source,0,0,SRCCOPY);
    if(previous)SelectObject(memory,previous);
    if(memory)DeleteDC(memory);ReleaseDC(window,source);
    if(!copied){if(bitmap)DeleteObject(bitmap);return nullptr;}
    size={r.right,r.bottom};return bitmap;
}
void paintWindowFrame(HWND window,HDC dc,HBITMAP bitmap,SIZE size){
    RECT r{};GetClientRect(window,&r);
    if(bitmap){
        if(auto memory=CreateCompatibleDC(dc)){
            auto previous=SelectObject(memory,bitmap);
            const bool copied=StretchBlt(dc,0,0,r.right,r.bottom,memory,0,0,size.cx,size.cy,SRCCOPY)!=FALSE;
            SelectObject(memory,previous);DeleteDC(memory);if(copied)return;
        }
    }
    FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}
bool certificateArgument(const void* value){
    // Supported client: WC_ITEM_POPUP's lpParam is the native XML string.
    // Inspect just the tag, before CreateWindowExA can consume the argument.
    MEMORY_BASIC_INFORMATION region{};
    if(!value||!VirtualQuery(value,&region,sizeof(region))||region.State!=MEM_COMMIT||(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
    const auto at=reinterpret_cast<UINT_PTR>(value),start=reinterpret_cast<UINT_PTR>(region.BaseAddress);
    if(at<start||region.RegionSize<8||at-start>region.RegionSize-8)return false;
    return !memcmp(value,"<NINTEI",7)&&(static_cast<const char*>(value)[7]==' '||static_cast<const char*>(value)[7]=='>');
}
std::string xmlAttribute(std::string_view xml,std::string_view name){
    const auto key=" "+std::string(name)+"=\"";auto at=xml.find(key);if(at==xml.npos)return {};at+=key.size();const auto end=xml.find('"',at);
    return end==xml.npos?std::string():std::string(xml.substr(at,end-at));
}
bool validGameToken(std::string_view token){
    return !token.empty()&&token.size()<=256&&token.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_")==token.npos;
}
std::wstring accountKey(std::wstring value){
    // The ID's hexadecimal prefix is case-insensitive; its secret suffix is not.
    if(value.size()==19&&value.substr(0,2)==L"ID")for(size_t i=2;i<10;++i)if(value[i]>=L'a'&&value[i]<=L'f')value[i]-=L'a'-L'A';
    return value;
}
class GameReturn {
    std::string gpid,hello;
    bool live=false,finished=false,requested=false,started=false,preparing=false,byeSent=false,helloSent=false,acknowledged=false,failed=false,reconnectRequired=false;
    unsigned attempts=0;
    ULONGLONG deadline=0,retryAt=0;
    DWORD helloSocket=MAXDWORD,gameSocket=MAXDWORD;
    static void clear(std::string& value){std::fill(value.begin(),value.end(),'\0');value.clear();}
public:
    void reset(){clear(gpid);clear(hello);live=finished=requested=started=preparing=byeSent=helloSent=acknowledged=failed=reconnectRequired=false;attempts=0;deadline=retryAt=0;helloSocket=gameSocket=MAXDWORD;}
    ~GameReturn(){reset();}
    void remember(std::string token){
        if(token.empty()||token.size()>256||token.find_first_of("\"<>& \r\n\t")!=token.npos)return;
        std::fill(gpid.begin(),gpid.end(),'\0');gpid=std::move(token);
    }
    void observe(std::string_view xml,bool viewer){
        if(!returnHooks)return;
        const auto tag=LobbyProtocol::Tag(xml);
        if(tag=="GO"){
            int type=0;const bool human=LobbyProtocol::Integer(xmlAttribute(xml,"type"),0,65535,type)&&(type&1);
            if(viewer||!human||xmlAttribute(xml,"kansen")=="1"){reset();return;}
            const auto token=xmlAttribute(xml,"gpid");
            // Root/view/game may observe the same GO. Do not restart the
            // transfer state on that duplicate delivery.
            if(!live||token!=gpid||(token.empty()&&helloSent)){reset();live=true;remember(token);gameSocket=nativeSocket?*nativeSocket:MAXDWORD;returnTrace("GO captured; token retained only in memory");}
        }else if(live&&(((tag=="AGARI"||tag=="RYUUKYOKU")&&!xmlAttribute(xml,"owari").empty())||tag=="PROF")){
            if(!finished){
                // /4 removes gpid at owari and closes the game connection.
                // It is a live-game resume token, not a post-game login token.
                clear(gpid);returnTrace("final result received; retired game GPID; waiting for result OK");
            }
            finished=true;
        }else if(helloSent&&!acknowledged&&tag=="HELO"&&(!xmlAttribute(xml,"uname").empty()||!xmlAttribute(xml,"PF4").empty()||!xmlAttribute(xml,"PF3").empty())){
            acknowledged=true;failed=false;retryAt=0;clear(hello);clear(gpid);returnTrace("lobby HELO accepted",attempts);
        }
    }
    bool completedGame()const{return live&&finished;}
    bool playing()const{return live&&!finished;}
    bool waitingForOK()const{return completedGame()&&!requested;}
    bool returning()const{return completedGame()&&requested;}
    bool prepared()const{return completedGame()&&started;}
    bool prefetching()const{return prepared()&&preparing;}
    bool duplicateReturn()const{return prepared()&&helloSent;}
    bool pending()const{return prepared()&&!acknowledged;}
    bool stopped()const{return pending()&&failed;}
    bool prepareReturn(){if(!completedGame()||started)return false;started=preparing=true;deadline=GetTickCount64()+20000;returnTrace("preparing return profile behind native result");return true;}
    void request(){if(!completedGame()||requested)return;requested=started=true;if(!deadline)deadline=GetTickCount64()+20000;returnTrace("result OK / table exit requested");}
    void stop(){if(pending()&&!failed){failed=true;retryAt=0;returnTrace("recovery stopped; ordinary lobby released",attempts);}}
    bool busyError(){
        if(!pending()||!helloSent){if(waitingForOK()){returnTrace("early ERR 1004 held on result screen");return true;}return false;}
        if(!failed&&!retryAt){
            returnTrace("ERR 1004; native popup/login fallback intercepted",attempts);
            // A rejected login must start a fresh native authentication flow.
            // Replaying HELO on that connection can lose its one-shot profile
            // attributes, including the rank certificate.
            reconnectRequired=true;
            if(attempts>=6||GetTickCount64()>=deadline)stop();
            else retryAt=GetTickCount64()+(500ull<<std::min(attempts?attempts-1:0u,3u));
        }
        return true;
    }
    void disconnected(){if(!pending()||failed)return;returnTrace("connection closed during return",attempts);retryAt=GetTickCount64()+500;}
    void rehelo(){
        reconnectRequired=true;
        // Before OK, remember the server request without destroying the table.
        // During handoff it is an explicit negative response, not a timeout.
        if(pending()&&!failed){if(attempts>=6)stop();else if(!retryAt)retryAt=GetTickCount64()+500;}
    }
    void connected(){if(!pending()||failed)return;helloSent=false;byeSent=true;retryAt=0;returnTrace("native replacement connection ready",attempts);}
    // Retry only after a negative reply / disconnect, never an unanswered HELO.
    bool poll(){if(!pending()||failed)return false;if(GetTickCount64()>=deadline){stop();return false;}if(!retryAt||GetTickCount64()<retryAt)return false;retryAt=0;return true;}
    const std::string& retryPacket()const{return hello;}
    bool needsReconnect()const{return reconnectRequired;}
    void reconnecting(){helloSent=false;byeSent=true;reconnectRequired=false;returnTrace("requesting native reconnect",attempts);}
    bool socketChanged()const{return helloSocket!=MAXDWORD&&nativeSocket&&helloSocket!=*nativeSocket;}
    bool needsBye()const{return prepared()&&!byeSent&&!helloSent;}
    bool needsFinalBye()const{return completedGame()&&!byeSent;}
    std::string prepare(std::string_view xml)const{
        if(!prepared())return std::string(xml);
        const auto tag=LobbyProtocol::Tag(xml);
        if(tag=="BYE")return byeSent||helloSent?std::string():std::string(xml);
        if(tag!="HELO")return std::string(xml);
        if(helloSent)return {};
        const auto original=xmlAttribute(xml,"gpid");
        returnTrace(original.empty()?"post-game HELO without resume token":"removed expired resume token from post-game HELO");
        returnTrace(nativeSocket&&*nativeSocket==gameSocket?"return uses game's socket":"return uses a different socket");
        std::string packet(xml);
        auto at=packet.find(" gpid=\"");
        if(at!=packet.npos){const auto end=packet.find('"',at+7);if(end!=packet.npos)packet.erase(at,end-at+1);}
        return packet;
    }
    void sent(std::string_view xml){
        if(!completedGame())return;
        const auto tag=LobbyProtocol::Tag(xml);
        if(tag=="BYE"){byeSent=true;returnTrace("BYE sent",attempts);}
        else if(prepared()&&tag=="HELO"){
            // Own the packet: the native builder frees its temporary on return.
            std::string copy(xml);clear(hello);hello=std::move(copy);
            helloSent=true;acknowledged=false;helloSocket=nativeSocket?*nativeSocket:MAXDWORD;++attempts;
            returnTrace("post-game HELO sent without GPID",attempts);
        }
    }
};
BOOL CALLBACK findGame(HWND w,LPARAM value){if(classIs(w,L"WC_GAME")&&visible(w))*reinterpret_cast<bool*>(value)=true;return TRUE;}
struct Session:std::enable_shared_from_this<Session> {
    HWND view=nullptr,host=nullptr,lobby=nullptr,welcome=nullptr;
    bool closing=false,ready=false,rendered=false,failed=false,loggedIn=false,loginPending=false,gamePainted=false,nativeRepaintPending=false;
    bool hostBoundsValid=false,controllerBoundsValid=false,controllerVisible=false,parentPositionValid=false;
    RECT hostBounds{},controllerBounds{};
    POINT parentPosition{};
    DWORD uiThread=GetCurrentThreadId();
    bool nativeViewer=false;
    std::string pendingViewerUrl;
    enum class Reconnect { None, Lobby, Login, Abandoned };
    Reconnect reconnect=Reconnect::None;
    bool disconnected=false;
    ULONGLONG reconnectDeadline=0;
    std::string reconnectLogin;
    bool reconnectHelloSent=false;
    bool reconnectPacketSent=false,reconnectResume=false,retryLoginQueued=false;
    ULONGLONG retryLoginAt=0;
    unsigned reconnectAttempts=0;
    bool nativeAuthPending=false;
    std::string gameResumeToken;
    std::wstring gameResumeAccount;
    GameReturn gameReturn;
    int queuedJoin=-1;
    unsigned joinGeneration=0;
    std::set<std::string> webReservations;
    bool awaitingTablePaint=false;
    struct Certificate {HWND window;bool shown;};
    std::vector<Certificate> certificates;
    std::string certificateRecord;
    bool certificateDelivered=false;
    std::string deferredReturnProfile;
    bool returnProfileReceived=false,returnNavigationReady=false;
    bool certificateInputBlocked=false,nativeCertificateActive=false,certificateDismissPending=false;
    bool nativeSettingsActive=false;
    // Trial (non-human) tables are not tracked by GameReturn. Remember one
    // from its GO until the native table has gone away.
    bool trialTable=false;
    // Set when the native/Web route is replaced for an authenticated account.
    // Until the replacement connection is accepted, ERR 1004 means the old
    // server session has not retired yet and is retried instead of shown.
    bool routeSwitch=false;
    bool legacyBackdrop=false,updatingLegacyBackdrop=false;
    std::vector<HWND> backdropHiddenWindows;
    bool certificatePosted=false,backgroundOnly=false;
    bool startupReady=false;
    unsigned startupSequence=0;
    HWND startupCover=nullptr;
    HBITMAP startupFrame=nullptr;
    SIZE startupFrameSize{};
    int returnStatus=-1;
    bool allowReturnConnect=false;
    unsigned nativeOpenSequence=0;
    std::deque<unsigned> pendingNativeOpens;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> browser;
    std::unordered_map<std::string,std::string> events;
    std::wstring account;std::string sex="M";
    std::deque<std::wstring> commands;
    std::future<LobbyServices::RegistrationResult> registration;
    std::future<LobbyServices::AccountLogsResult> accountLogs;
    std::future<LobbyServices::WatchListResult> watchList;
    int watchSequence=0;
    int queuedAccountSequence=0;
    ~Session(){clearGameResume();clearReturnProfile();if(startupFrame)DeleteObject(startupFrame);}
    void updateStartupCover(){
        if(!startupFrame)return;
        if(startupReady||failed||closing){
            if(IsWindow(startupCover))ShowWindow(startupCover,SW_HIDE);
            DeleteObject(startupFrame);startupFrame=nullptr;return;
        }
        RECT r{},cover{};GetClientRect(GetAncestor(view,GA_ROOT),&r);GetClientRect(startupCover,&cover);
        if(!EqualRect(&r,&cover)||!(GetWindowLongPtrW(startupCover,GWL_STYLE)&WS_VISIBLE)||GetWindow(startupCover,GW_HWNDPREV))
            SetWindowPos(startupCover,HWND_TOP,0,0,r.right,r.bottom,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    void captureStartupFrame(){
        if(startupReady||startupFrame||failed||closing)return;
        auto root=GetAncestor(view,GA_ROOT);
        auto bitmap=captureWindowFrame(root,startupFrameSize);if(!bitmap)return;
        startupCover=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPSIBLINGS,0,0,startupFrameSize.cx,startupFrameSize.cy,root,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!startupCover||!SetWindowSubclass(startupCover,coverObserver,viewSubclass,reinterpret_cast<DWORD_PTR>(view))){
            if(startupCover)DestroyWindow(startupCover);startupCover=nullptr;DeleteObject(bitmap);return;
        }
        startupFrame=bitmap;updateStartupCover();
        diagnostic("Web lobby: loading frame retained until login first paint.");
    }
    void paintCover(HWND window,HDC dc){
        paintWindowFrame(window,dc,window==startupCover?startupFrame:nullptr,startupFrameSize);
    }
    std::wstring roomCode()const{wchar_t value[32]{};GetPrivateProfileStringW(L"STATUS",L"LOBBY_LAST",L"L0000",value,32,(directory+L"\\config.ini").c_str());return value;}
    int lobbyId()const {
        wchar_t last[32]{};GetPrivateProfileStringW(L"STATUS",L"LOBBY_LAST",L"L0000",last,32,(directory+L"\\config.ini").c_str());
        std::wstring value=last;int id=0;
        if(value.size()==5&&value[0]==L'L'&&LobbyProtocol::Integer(LobbyProtocol::Utf8(value.substr(1)),0,9999,id))return id;
        if(value.size()==9&&value[0]==L'C'&&LobbyProtocol::Integer(LobbyProtocol::Utf8(value.substr(1,4)),0,9999,id))return 10000+id;
        return 0;
    }
    void post(const std::string& json){if(browser&&ready){auto wide=LobbyProtocol::Wide(json);if(!wide.empty())browser->PostWebMessageAsJson(wide.c_str());}}
    void error(const char* message,bool resetJoin=false,const char* messageKey=""){post("{\"type\":\"error\",\"resetJoin\":"+std::string(resetJoin?"true":"false")+",\"message\":"+LobbyProtocol::Quote(message)+",\"messageKey\":"+LobbyProtocol::Quote(messageKey)+"}");}
    void sendEvent(const std::string& xml){post("{\"type\":\"xml\",\"xml\":"+LobbyProtocol::Quote(xml)+"}");}
    void loginState(bool pending){loginPending=pending;post(std::string("{\"type\":\"loginState\",\"pending\":")+(pending?"true":"false")+"}");}
    bool connectionUnavailable()const{return disconnected||!nativeSocket||!LobbyConnection::Ready(*nativeSocket);}
    void clearReconnect(){reconnect=Reconnect::None;reconnectDeadline=retryLoginAt=0;reconnectAttempts=0;reconnectHelloSent=reconnectPacketSent=reconnectResume=retryLoginQueued=nativeAuthPending=false;std::fill(reconnectLogin.begin(),reconnectLogin.end(),'\0');reconnectLogin.clear();}
    void clearGameResume(){std::fill(gameResumeToken.begin(),gameResumeToken.end(),'\0');gameResumeToken.clear();gameResumeAccount.clear();}
    bool canResume(const std::wstring& id)const{return !gameResumeToken.empty()&&accountKey(id)==gameResumeAccount;}
    bool nativeSettingsBlocked()const{
        // The table can disappear during disconnect recovery. Its retained
        // resume token still identifies an unfinished game after reset().
        return gameReturn.playing()||!gameResumeToken.empty()||awaitingTablePaint||trialTable||
            reconnect==Reconnect::Login||reconnect==Reconnect::Lobby;
    }
    void refreshTrialTable(){
        if(!trialTable||awaitingTablePaint)return;
        bool game=false;EnumChildWindows(view,findGame,reinterpret_cast<LPARAM>(&game));
        if(!game)trialTable=false;
    }
    void updateSettingsMenu(){
        refreshTrialTable();
        if(auto menu=GetSystemMenu(GetAncestor(view,GA_ROOT),FALSE))
            EnableMenuItem(menu,nativeSettingsCommand,MF_BYCOMMAND|(nativeSettingsBlocked()?MF_GRAYED:MF_ENABLED));
    }
    void clearReservations(){queuedJoin=-1;++joinGeneration;webReservations.clear();}
    void failReconnect(){
        awaitingTablePaint=false;
        pendingViewerUrl.clear();WebTransport::SetActive(false);
        clearReconnect();reconnect=Reconnect::Abandoned;disconnected=true;clearReservations();loginState(false);
        if(IsWindow(welcome))EnableWindow(GetDlgItem(welcome,1),TRUE);
        error("连接未能恢复，请检查网络后重试。",true);
        diagnostic("Web lobby: reconnect failed; controls released for retry.");
        // Retire asynchronously: this can be called from the network window's
        // DNS/FD_CONNECT callback, before its original procedure has returned.
        auto network=GetDlgItem(GetAncestor(view,GA_ROOT),3006);
        if(classIs(network,L"WC_CLIENT"))PostMessageW(view,retireNetworkMessage,reinterpret_cast<WPARAM>(network),0);
    }
    void disconnectedLogin(){
        // The native modal can be owned by a child window. Deliver its YES
        // action to this session's view, then navigate through the root.
        disconnected=true;clearReconnect();gameReturn.reset();clearReturnProfile();
        awaitingTablePaint=false;nativeViewer=false;
        pendingViewerUrl.clear();WebTransport::SetActive(false);
        clearReservations();commands.clear();pendingNativeOpens.clear();
        returnStatus=-1;postReturnState();
        const auto root=GetAncestor(view,GA_ROOT);
        // Retire the disconnected window (including its polling timer) before
        // native table cleanup can run on the next login's global socket.
        if(auto network=GetDlgItem(root,3006);classIs(network,L"WC_CLIENT"))DestroyWindow(network);
        // The native navigation must reach welcome without auto-login. Keep
        // the live game's token in this session for the next explicit OK;
        // consuming the native one-shot GPID here must not abandon that game.
        WritePrivateProfileStringW(L"STATUS",L"GPID",nullptr,(directory+L"\\config.ini").c_str());
        diagnostic("Web lobby: disconnect YES handled by view; returning to login.");
        SendMessageA(root,0x8204,0,reinterpret_cast<LPARAM>("GOWELCOMEBYERR"));
    }
    // The server refuses to resume an unfinished game after too many drops.
    // Resume is therefore best effort: once retries are exhausted, discard the
    // token and log in normally so the user lands in the lobby, not on a
    // login screen that can never succeed.
    void expireReconnect(){
        if(reconnect!=Reconnect::Login||!reconnectResume||gameResumeToken.empty()){failReconnect();return;}
        connectionTrace("game resume unavailable; falling back to normal lobby login",reconnectAttempts);
        clearGameResume();reconnectResume=false;
        // Retire the lobby session that refused the resume; otherwise the normal
        // login races it and hits ERR 1004 ("already logged in"), now retried silently.
        if(!connectionUnavailable())nativeSend("<BYE />");
        routeSwitch=true;
        reconnectAttempts=0;reconnectDeadline=GetTickCount64()+20000;
        loggedIn=false;events.clear();clearReservations();
        nativeAuthPending=false;retryLoginQueued=true;retryLoginAt=GetTickCount64();
        auto network=GetDlgItem(GetAncestor(view,GA_ROOT),3006);
        if(classIs(network,L"WC_CLIENT"))PostMessageW(view,retireNetworkMessage,reinterpret_cast<WPARAM>(network),0);
    }
    void beginReconnect(Reconnect target){
        const bool retry=retryLoginQueued&&reconnect==target;
        if(!retry&&(reconnect==Reconnect::Lobby||reconnect==Reconnect::Login))return;
        if(retry&&(GetTickCount64()>=reconnectDeadline||reconnectAttempts>=6)){expireReconnect();return;}
        ++joinGeneration;webReservations.clear();
        retryLoginQueued=false;
        nativeAuthPending=false;
        gameReturn.reset();clearReturnProfile();
        disconnected=true;reconnect=target;
        if(!retry){reconnectDeadline=GetTickCount64()+20000;reconnectAttempts=0;}
        ++reconnectAttempts;
        reconnectHelloSent=false;
        if(target==Reconnect::Login){
            const auto split=reconnectLogin.find('\n');
            reconnectResume=canResume(LobbyProtocol::Wide(reconnectLogin.substr(0,split)));
        }
        diagnostic(target==Reconnect::Login?"Web lobby: rebuilding connection before login.":"Web lobby: rebuilding connection before reservation.");
        // CONNECT destroys the native network window and creates a new one.
        // Merely navigating to GOWELCOME leaves the old, closed socket behind.
        SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("CONNECT"));
    }
    void switchRoute(bool useWeb){
        // Log the account out of the current connection before replacing it.
        // Merely closing the socket leaves the server session alive, so the
        // replacement HELO is rejected with ERR 1004 (already connected).
        if(loggedIn&&!connectionUnavailable()&&nativeSend("<BYE />")==8)connectionTrace("BYE sent before route switch");
        WebTransport::SetActive(useWeb);routeSwitch=true;
    }
    bool retryRouteSwitch(){
        if(!routeSwitch||(reconnect!=Reconnect::Lobby&&reconnect!=Reconnect::Login))return false;
        if(retryLoginQueued)return true;
        connectionTrace("ERR 1004 after route switch; waiting for previous session to retire",reconnectAttempts);
        if(reconnectAttempts>=6||GetTickCount64()>=reconnectDeadline){failReconnect();return true;}
        // Keep queuedJoin/reconnectLogin: the retry replays the same intent.
        retryLoginQueued=true;nativeAuthPending=false;
        retryLoginAt=GetTickCount64()+(500ULL<<std::min(reconnectAttempts-1,3u));
        auto network=GetDlgItem(GetAncestor(view,GA_ROOT),3006);
        if(classIs(network,L"WC_CLIENT"))PostMessageW(view,retireNetworkMessage,reinterpret_cast<WPARAM>(network),0);
        return true;
    }
    void returnToNative(std::string_view url={}){
        pendingViewerUrl.assign(url);
        nativeViewer=!url.empty();
        clearReservations();clearReconnect();
        switchRoute(false);
        reconnectLogin=LobbyProtocol::Utf8(account.empty()?L"NoName":account)+"\n"+sex;
        diagnostic("Web lobby: restoring original Windows lobby/viewer connection.");
        beginReconnect(Reconnect::Login);
    }
    void openPendingViewer(){
        if(pendingViewerUrl.empty()||reconnect!=Reconnect::None||connectionUnavailable()||WebTransport::Active())return;
        std::string url;url.swap(pendingViewerUrl);
        SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>(url.c_str()));
    }
    void sendReconnectLogin(){
        if(reconnect!=Reconnect::Login||reconnectHelloSent)return;
        const auto split=reconnectLogin.find('\n');
        const auto id=reconnectLogin.substr(0,split);
        if(id!="NoName"&&!std::regex_match(id,std::regex("ID[0-9A-Fa-f]{8}-[0-9A-Za-z]{8}"))){failReconnect();return;}
        account=LobbyProtocol::Wide(id);sex=split!=reconnectLogin.npos&&reconnectLogin.substr(split+1)=="F"?"F":"M";
        const bool resume=canResume(account);
        const auto ini=directory+L"\\config.ini";
        if(!WritePrivateProfileStringW(L"STATUS",L"UNAME",account.c_str(),ini.c_str())||
           !WritePrivateProfileStringW(L"STATUS",L"SEXUALITY",sex=="F"?L"F":L"M",ini.c_str())){failReconnect();return;}
        const auto token=LobbyProtocol::Wide(gameResumeToken);
        if(!WritePrivateProfileStringW(L"STATUS",L"GPID",resume?token.c_str():nullptr,ini.c_str())){failReconnect();return;}
        // Use the supported executable's own HELO builder, including its
        // resume attribute ordering and one-shot GPID consumption. GOLOBBY
        // sends it without clicking a disabled/recreated welcome button.
        reconnectHelloSent=true;
        reconnectPacketSent=false;
        connectionTrace(resume?"native token resume requested":"native normal login requested");
        SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("GOLOBBY"));
        if(reconnect==Reconnect::Login&&!reconnectPacketSent)failReconnect();
    }
    bool retryGameResume(const char* reason,unsigned limit=6){
        if(reconnect!=Reconnect::Login||!reconnectResume||!loginPending||!canResume(account))return false;
        if(retryLoginQueued)return true;
        connectionTrace(reason,reconnectAttempts);
        if(reconnectAttempts>=limit||GetTickCount64()>=reconnectDeadline){expireReconnect();return true;}
        // Keep the same account/token and the original deadline. Closing the
        // rejected connection precedes a bounded delay, never another modal.
        retryLoginQueued=true;nativeAuthPending=false;loggedIn=false;events.clear();clearReservations();
        retryLoginAt=GetTickCount64()+(500ULL<<std::min(reconnectAttempts-1,3u));
        auto network=GetDlgItem(GetAncestor(view,GA_ROOT),3006);
        if(classIs(network,L"WC_CLIENT"))PostMessageW(view,retireNetworkMessage,reinterpret_cast<WPARAM>(network),0);
        return true;
    }
    bool recoveryProfile(std::string_view xml){
        const auto tag=LobbyProtocol::Tag(xml);
        if(tag=="HELO"&&!xmlAttribute(xml,"auth").empty()){
            nativeAuthPending=true;connectionTrace("native authentication challenge received; awaiting GO or LN");return false;
        }
        const bool lobbyReply=(tag=="LN"&&nativeAuthPending)||(tag=="HELO"&&
            (!xmlAttribute(xml,"uname").empty()||!xmlAttribute(xml,"PF4").empty()||!xmlAttribute(xml,"PF3").empty()));
        if(reconnect!=Reconnect::Login||!reconnectHelloSent||!reconnectResume||!lobbyReply)return false;
        // A lobby profile is not proof that the unfinished game ended. Never
        // strip its GPID or permit a second reservation on that assumption.
        // The server answered with a lobby profile: a second answer means the
        // resume limit was reached, so do not keep retrying.
        return retryGameResume("resume returned lobby without resolving unfinished game",2);
    }
    bool rejoin(std::string_view xml){
        if(!WebTransport::Active())return false;
        const auto reservation=xmlAttribute(xml,"t");auto key=reservation;
        if(key.size()>2&&key.compare(key.size()-2,2,",r")==0)key.resize(key.size()-2);
        if(webReservations.count(key)){
            const auto packet="<JOIN t=\""+reservation+"\" />";
            if(nativeSend(packet.c_str())!=static_cast<int>(packet.size()+1)){
                clearReservations();error("重新预约发送失败，请重试。",true);
            }else diagnostic("Web lobby: server REJOIN reservation resubmitted.");
        }else diagnostic("Web lobby: stale/cancelled REJOIN ignored.");
        return true;
    }
    void publishBackground(){
        post("{\"type\":\"nativeReturn\",\"active\":"+std::string(backgroundOnly?"true":"false")+"}");
    }
    void setBackgroundOnly(bool active){if(backgroundOnly==active)return;backgroundOnly=active;publishBackground();}
    void clearReturnProfile(){
        std::fill(deferredReturnProfile.begin(),deferredReturnProfile.end(),'\0');deferredReturnProfile.clear();
        returnProfileReceived=returnNavigationReady=false;
    }
    void prepareReturn(){
        if(!returnPrefetchHooks||!gameReturn.waitingForOK()||!gameReturn.prepareReturn())return;
        // The old WC_CLIENT has finished destruction. Reconnect now while
        // the user reads the real native score page, without dismissing it.
        WebTransport::SetActive(false);allowReturnConnect=true;
        SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("CONNECT"));
        allowReturnConnect=false;
    }
    void retainReturnProfile(std::string_view xml){
        std::fill(deferredReturnProfile.begin(),deferredReturnProfile.end(),'\0');deferredReturnProfile.assign(xml);
        // Native AUTH is answered on the original ingress. Replaying the
        // profile for its windows must not send that challenge a second time.
        const auto auth=deferredReturnProfile.find(" auth=\"");
        if(auth!=std::string::npos){const auto end=deferredReturnProfile.find('"',auth+7);if(end!=std::string::npos)deferredReturnProfile.erase(auth,end-auth+1);}
        returnProfileReceived=true;
    }
    void finishReturn(){
        if(!gameReturn.returning()||!returnNavigationReady)return;
        // Run after the result button's native callback has unwound: applying
        // HELO can destroy its WC_GAME and the callback's native objects.
        if(!deferredReturnProfile.empty()){
            returnTrace("applying retained profile after native GOLOBBY");
            std::string profile;profile.swap(deferredReturnProfile);
            SendMessageA(GetAncestor(view,GA_ROOT),0x8209,0,reinterpret_cast<LPARAM>(profile.c_str()));
            std::fill(profile.begin(),profile.end(),'\0');
        }
        restoreCertificate();show();
    }
    void requestReturn(){
        gameReturn.request();
        if(gameReturn.returning()){
            // The BYE hook runs before native result dismissal finishes. Let
            // its posted GOLOBBY replay the profile after that callback returns.
            if(loggedIn)synchronize();
            post("{\"type\":\"returnLobby\"}");show();
        }
    }
    void rememberCertificate(std::string_view xml){
        const auto tag=LobbyProtocol::Tag(xml);
        if(tag!="HELO"&&tag!="PROF"&&tag!="NINTEI")return;
        const auto value=xmlAttribute(xml,"nintei");
        if(value.empty()||value.size()>65536)return;
        const auto record="<NINTEI nintei=\""+value+"\" />";
        if(record==certificateRecord)return;
        certificateRecord=record;certificateDelivered=false;
        returnTrace("server rank certificate retained before native profile handling");
    }
    void restoreCertificate(){
        if(!returnNavigationReady||!deferredReturnProfile.empty()||certificateRecord.empty()||certificateDelivered)return;
        // Native HELO normally creates this popup itself. An AUTH challenge
        // or a final PROF can arrive before WC_LOBBY exists; retain only its
        // certificate and use the same native popup after result dismissal.
        const auto root=GetAncestor(view,GA_ROOT);
        const auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(view,GWLP_HINSTANCE));
        const auto window=previousCreate(0,"WC_ITEM_POPUP",nullptr,WS_CHILD|WS_CLIPSIBLINGS,0,0,0,0,root,nullptr,instance,certificateRecord.data());
        if(!window){returnTrace("retained rank certificate creation failed");return;}
        attachCertificate(window);
        returnTrace("retained rank certificate restored through native popup");
    }
    void queueCertificate(){
        // Give the original HELO handler first chance to create its popup.
        if(gameReturn.returning()&&returnNavigationReady&&deferredReturnProfile.empty()&&!certificateRecord.empty()&&!certificateDelivered&&!certificatePosted)
            certificatePosted=PostMessageW(view,nativeCertificateReadyMessage,0,0)!=FALSE;
    }
    void showCertificate(){certificatePosted=false;if(gameReturn.returning())restoreCertificate();show();}
    void postReturnState(){
        const int state=gameReturn.stopped()?2:gameReturn.pending()?1:0;
        if(state==returnStatus)return;returnStatus=state;
        post("{\"type\":\"returnState\",\"state\":"+std::to_string(state)+"}");
    }
    void pollReturn(){
        if(gameReturn.poll()){
            if(!nativeSocket||*nativeSocket==MAXDWORD||gameReturn.needsReconnect()||gameReturn.retryPacket().empty()||gameReturn.socketChanged()){
                WebTransport::SetActive(false);gameReturn.reconnecting();allowReturnConnect=true;
                SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("CONNECT"));
                allowReturnConnect=false;
            }else{
                const auto& packet=gameReturn.retryPacket();const int sent=nativeSend(packet.c_str());
                if(sent==static_cast<int>(packet.size()+1))gameReturn.sent(packet);else gameReturn.stop();
            }
        }
        if(gameReturn.stopped()){queuedJoin=-1;}
        postReturnState();
    }
    void observeConnection(std::string_view xml){
        const auto tag=LobbyProtocol::Tag(xml);
        if(tag=="UN"&&gameReturn.playing()&&!nativeViewer&&!loggedIn){
            // A native token resume can go straight to GO/UN/REINIT without
            // a lobby HELO. UN's n0 is the authenticated player's public name;
            // restore that identity without inventing missing aggregate stats
            // or exposing the account ID to the browser.
            const auto name=xmlAttribute(xml,"n0");
            if(!name.empty()){
                const auto cached=events.find("HELO");
                const bool namedProfile=cached!=events.end()&&!xmlAttribute(cached->second,"uname").empty();
                const auto profile=namedProfile?cached->second:"<PROF uname=\""+name+"\" />";
                if(!namedProfile)events["PROF"]=profile;
                loggedIn=true;sendEvent(profile);
                connectionTrace("authenticated game identity restored for resumed lobby");
            }
        }
        if(LobbyProtocol::Tag(xml)=="GO"){
            int type=0;
            const bool typed=LobbyProtocol::Integer(xmlAttribute(xml,"type"),0,65535,type);
            trialTable=!nativeViewer&&typed&&!(type&1)&&xmlAttribute(xml,"kansen")!="1";
            if(!nativeViewer&&typed&&(type&1)&&xmlAttribute(xml,"kansen")!="1"){
                const auto token=xmlAttribute(xml,"gpid");
                if(validGameToken(token)){
                    gameResumeToken=token;gameResumeAccount=accountKey(account.empty()?L"NoName":account);
                }
            }
            // A reconnect may resume an assigned table instead of receiving
            // a lobby HELO. Never queue a second match or time out that game.
            awaitingTablePaint=true;disconnected=false;clearReconnect();routeSwitch=false;clearReservations();
            // GO only assigns the table; keep the login button pending until
            // the original table has actually painted over the welcome page.
        }
        gameReturn.observe(xml,nativeViewer);
        if(gameReturn.completedGame())clearGameResume();
        if(LobbyProtocol::Tag(xml)=="GO"){certificateRecord.clear();certificateDelivered=false;clearReturnProfile();}
        if(LobbyProtocol::Tag(xml)!="ERR")return;
        awaitingTablePaint=false;
        clearReservations();
        if(reconnect==Reconnect::Login){clearReconnect();disconnected=true;}
        // Live recovery's 1004 is consumed at ingress. Other server errors
        // retain the original client's error/exit behavior.
        if(gameReturn.pending()&&xmlAttribute(xml,"code")!="1004"){gameReturn.reset();clearReturnProfile();}
        // The original client owns the error dialog and disconnection. Only
        // release the WebView's pending controls, without duplicating the error.
        if(loginPending)loginState(false);
        auto at=xml.find(" code=\"");if(at==xml.npos)return;at+=7;auto end=xml.find('"',at);int code=0;
        if(end!=xml.npos&&LobbyProtocol::Integer(xml.substr(at,end-at),1000,9999,code)){char line[100];sprintf_s(line,"Web lobby: native ERR %d (%s).",code,loggedIn?"authenticated":"login");diagnostic(line);}
    }
    void observe(std::string_view xml) {
        rememberCertificate(xml);
        const auto incomingTag=LobbyProtocol::Tag(xml);
        if(incomingTag=="HELO"&&!xmlAttribute(xml,"auth").empty()){
            // Native HELO may carry PF4/uname *and* an AUTH challenge. Cache
            // the public profile, but let the original client answer AUTH;
            // neither the browser nor queued JOIN can treat it as a login ACK.
            nativeAuthPending=true;
            auto profile=LobbyProtocol::FilterEvent(xml);if(!profile.empty())events["HELO"]=std::move(profile);else events.erase("HELO");
            return;
        }
        if(incomingTag=="LN"&&nativeAuthPending){
            nativeAuthPending=false;
            const auto cached=events.find("HELO");
            if(cached!=events.end()){const auto profile=cached->second;observe(profile);}
        }
        observeConnection(xml);
        auto filtered=LobbyProtocol::FilterEvent(xml);if(filtered.empty())return;
        const auto tag=LobbyProtocol::Tag(filtered);
        const bool profile=(tag=="HELO"||tag=="PROF")&&(!xmlAttribute(filtered,"uname").empty()||!xmlAttribute(filtered,"PF4").empty()||!xmlAttribute(filtered,"PF3").empty());
        if(profile){
            loggedIn=true;
            if(tag=="HELO"){
                disconnected=false;clearReconnect();routeSwitch=false;
                if(!pendingViewerUrl.empty())PostMessageW(view,openNativeViewerMessage,0,0);
            }
            if(loginPending&&!awaitingTablePaint)loginState(false);
            if(tag=="HELO"&&!gameReturn.pending()&&queuedJoin>=0){
                // Wait for an authenticated profile, not just FD_CONNECT or
                // an authentication challenge, before sending the reservation.
                PostMessageW(view,resumeJoinMessage,joinGeneration,0);
            }
        }
        // An identical profile still acknowledges a new connection.
        if((tag=="LN"||tag=="HELO"||tag=="PROF")&&events.count(tag)&&events[tag]==filtered)return;
        if(tag=="HELO"&&(!xmlAttribute(filtered,"PF4").empty()||!xmlAttribute(filtered,"PF3").empty()))events.erase("PROF");
        if(profile&&!events.count(tag))diagnostic("Web lobby: native profile event received.");
        if(profile||tag=="LN"||tag=="CS"||tag=="DATE"||tag=="RANKING")events[tag]=filtered;
        sendEvent(filtered);
    }
    void synchronize() {
        publishBackground();
        returnStatus=-1;postReturnState();
        std::string snapshot="{\"type\":\"snapshot\",\"id\":"+std::to_string(lobbyId())+",\"code\":"+LobbyProtocol::Quote(LobbyProtocol::Utf8(roomCode()))+",\"watchFlags\":"+std::to_string(LobbyProtocol::NormalizeWatchFlags(GetPrivateProfileIntW(L"STATUS",L"LOBBY_WG_FLAGS",defaultWatchFlags,(directory+L"\\config.ini").c_str())))+",\"events\":[";
        bool first=true;
        if(loggedIn)for(const auto& tag:{"HELO","PROF","LN","CS","DATE","RANKING"}){
            auto event=events.find(tag);if(event==events.end())continue;
            if(!first)snapshot+=',';first=false;snapshot+=LobbyProtocol::Quote(event->second);
        }
        const bool hideGuestJoukyuuTables=GetPrivateProfileIntW(L"lobby",L"hide_guest_joukyuu_tables",1,(directory+L"\\enhanced.ini").c_str())!=0;
        post(snapshot+"],\"web3\":"+std::string(WebTransport::Enabled()?"true":"false")+",\"hideGuestJoukyuuTables\":"+(hideGuestJoukyuuTables?"true":"false")+"}");
        options();
        if(!loggedIn)sendWelcome();
    }
    void rememberWelcome(HWND window){
        welcome=window;wchar_t value[256]{};const auto ini=directory+L"\\config.ini";
        // HIDDENID replaces the native edit text with a mask, not the account.
        if(GetPrivateProfileIntW(L"STATUS",L"HIDDENID",0,ini.c_str()))
            GetPrivateProfileStringW(L"STATUS",L"UNAME",L"",value,256,ini.c_str());
        else GetWindowTextW(GetDlgItem(window,3002),value,256);
        if(*value)account=value;
        wchar_t label[32]{};GetWindowTextW(GetDlgItem(window,3003),label,32);sex=wcschr(label,L'女')?"F":"M";
    }
    void sendWelcome(){
        if(gameReturn.waitingForOK()||gameReturn.pending()||(gameReturn.returning()&&loggedIn)){
            // Native welcome controls can be recreated by reconnect. They do
            // not mean that the authenticated lobby snapshot was logged out.
            post("{\"type\":\"returnLobby\"}");return;
        }
        post("{\"type\":\"welcome\",\"web3\":"+std::string(WebTransport::Enabled()?"true":"false")+",\"account\":"+LobbyProtocol::Quote(LobbyProtocol::Utf8(account))+",\"sex\":"+LobbyProtocol::Quote(sex)+"}");
    }
    void pollLogin(){
        if(awaitingTablePaint)return;
        if(reconnect==Reconnect::Abandoned)return;
        if(reconnect!=Reconnect::None){
            if(GetTickCount64()>=reconnectDeadline)expireReconnect();
            else if(retryLoginQueued&&GetTickCount64()>=retryLoginAt)beginReconnect(reconnect);
            return;
        }
        // Native login failures without an XML ERR (e.g. a socket failure)
        // restore the original OK control. Use that completion signal too.
        if(loginPending&&IsWindow(welcome)&&IsWindowEnabled(GetDlgItem(welcome,1)))loginState(false);
    }
    void pollRegistration(){if(registration.valid()&&registration.wait_for(std::chrono::seconds(0))==std::future_status::ready){auto result=registration.get();post("{\"type\":\"registerResult\",\"sequence\":"+std::to_string(result.sequence)+",\"response\":"+LobbyProtocol::Quote(result.response)+",\"error\":"+LobbyProtocol::Quote(result.error)+"}");}}
    void requestAccountLogs(int sequence){
        if(!loggedIn||!std::regex_match(account,std::wregex(L"ID[0-9A-F]{8}-[0-9A-Za-z]{8}"))){post("{\"type\":\"accountLogs\",\"sequence\":"+std::to_string(sequence)+",\"error\":true}");return;}
        if(accountLogs.valid()){queuedAccountSequence=sequence;return;}
        accountLogs=std::async(std::launch::async,LobbyServices::FindAccountLogs,sequence,account);
    }
    void pollAccountLogs(){
        if(accountLogs.valid()&&accountLogs.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            auto result=accountLogs.get();
            if(loggedIn&&result.account==account)post("{\"type\":\"accountLogs\",\"sequence\":"+std::to_string(result.sequence)+",\"response\":"+LobbyProtocol::Quote(result.response)+",\"error\":"+(result.error?"true":"false")+"}");
            if(queuedAccountSequence){const int sequence=queuedAccountSequence;queuedAccountSequence=0;requestAccountLogs(sequence);}
        }
    }
    void requestWatch(int sequence){
        watchSequence=sequence;
        const int room=lobbyId();
        if(!watchList.valid())watchList=std::async(std::launch::async,LobbyServices::FetchWatchList,room);
    }
    void pollWatch(){
        if(!watchList.valid()||watchList.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        auto result=watchList.get();
        if(!loggedIn)return;
        if(result.room!=lobbyId()){requestWatch(watchSequence);return;}
        post("{\"type\":\"watch\",\"sequence\":"+std::to_string(watchSequence)+",\"room\":"+std::to_string(result.room)+",\"source\":"+LobbyProtocol::Quote(result.response)+",\"error\":"+(result.error?"true":"false")+"}");
    }
    void options(){
        auto ini=directory+L"\\config.ini";auto flags=GetPrivateProfileIntW(L"STATUS",L"FLAGS0",0x4c2d,ini.c_str());
        const auto volume=std::min<UINT>(GetPrivateProfileIntW(L"RESOURCE",L"SE_VOL",100,ini.c_str()),200);
        wchar_t naki[32768]{};GetPrivateProfileStringW(L"RESOURCE",L"SE_NAKI",L"",naki,32768,ini.c_str());
        post("{\"type\":\"options\",\"values\":{\"c53\":"+std::to_string((volume+10)/20)+",\"c58\":"+std::to_string(GetPrivateProfileIntW(L"STATUS",L"HIDDENID",0,ini.c_str())!=0)+",\"c67\":"+std::to_string(_wcsicmp(naki,L"none")!=0)+",\"c68\":"+std::to_string((flags&0x8000)!=0)+",\"c69\":"+std::to_string(!(flags&0x200))+",\"c70\":"+std::to_string((flags&0x20)!=0)+",\"c71\":"+std::to_string(!(flags&0x800))+",\"c72\":"+std::to_string(!(flags&0x400))+",\"c73\":"+std::to_string(GetPrivateProfileIntW(L"STATUS",L"ESC_PASS",0,ini.c_str())!=0)+",\"c74\":"+std::to_string(GetPrivateProfileIntW(L"STATUS",L"ESC_DISCARD",0,ini.c_str())!=0)+"}}");
    }
    void option(const std::string& payload){
        auto split=payload.find('\n');if(split==payload.npos)return;auto id=payload.substr(0,split),value=payload.substr(split+1);if(value!="0"&&value!="1")return;const bool enabled=value=="1";auto ini=directory+L"\\config.ini";
        if(id=="c58"){WritePrivateProfileStringW(L"STATUS",L"HIDDENID",enabled?L"1":L"0",ini.c_str());return;}
        if(id=="c67"){
            wchar_t current[32768]{},previous[32768]{};GetPrivateProfileStringW(L"RESOURCE",L"SE_NAKI",L"",current,32768,ini.c_str());
            if(enabled==(_wcsicmp(current,L"none")!=0))return;
            if(!enabled){
                WritePrivateProfileStringW(L"DESKTOP",L"SE_NAKI_LAST",*current?current:nullptr,ini.c_str());
                WritePrivateProfileStringW(L"RESOURCE",L"SE_NAKI",L"none",ini.c_str());
            }else{
                GetPrivateProfileStringW(L"DESKTOP",L"SE_NAKI_LAST",L"",previous,32768,ini.c_str());
                if(!_wcsicmp(previous,L"none"))*previous=0;
                WritePrivateProfileStringW(L"RESOURCE",L"SE_NAKI",*previous?previous:nullptr,ini.c_str());
            }
            const wchar_t* source=enabled?previous:L"none";
            // Same functions and sound slot as the native SE page's None,
            // Default and file picker. Update the loaded sound immediately.
            if(enabled&&!*source){using DefaultSound=void(__cdecl*)(int);reinterpret_cast<DefaultSound>(reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr))+0x695b0)(0x6a);}
            else{
                const int size=WideCharToMultiByte(CP_ACP,0,source,-1,nullptr,0,nullptr,nullptr);std::string path(size,'\0');WideCharToMultiByte(CP_ACP,0,source,-1,path.data(),size,nullptr,nullptr);
                using LoadSound=void(__cdecl*)(int,const char*);reinterpret_cast<LoadSound>(reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr))+0x6e4e0)(0x6a,path.c_str());
            }
            return;
        }
        if(id=="c73"||id=="c74"){WritePrivateProfileStringW(L"STATUS",id=="c73"?L"ESC_PASS":L"ESC_DISCARD",enabled?L"1":L"0",ini.c_str());return;}
        unsigned mask=0;bool inverse=false;if(id=="c68")mask=0x8000;else if(id=="c69"){mask=0x200;inverse=true;}else if(id=="c70")mask=0x20;else if(id=="c71"){mask=0x800;inverse=true;}else if(id=="c72"){mask=0x400;inverse=true;}else return;
        auto flags=GetPrivateProfileIntW(L"STATUS",L"FLAGS0",0x4c2d,ini.c_str());flags=(flags&~mask)|((enabled!=inverse)?mask:0);WritePrivateProfileStringW(L"STATUS",L"FLAGS0",std::to_wstring(flags).c_str(),ini.c_str());
    }
    void room(const std::string& code){
        if(!std::regex_match(code,std::regex("(?:L[0-9]{4}|C[0-9]{8})")))return;
        auto wide=LobbyProtocol::Wide(code);WritePrivateProfileStringW(L"STATUS",L"LOBBY_LAST",wide.c_str(),(directory+L"\\config.ini").c_str());
        if(loggedIn){auto xml=code[0]=='C'?"<CS lobby=\""+code+"\" />":"<CHAT text=\"%2Flobby%20"+std::to_string(std::stoi(code.substr(1)))+"\" />";nativeSend(xml.c_str());}
    }
    void rememberBackdropWindow(HWND window){
        if(std::find(backdropHiddenWindows.begin(),backdropHiddenWindows.end(),window)==backdropHiddenWindows.end())
            backdropHiddenWindows.push_back(window);
    }
    void updateLegacyBackdrop(bool active){
        if(updatingLegacyBackdrop)return;
        updatingLegacyBackdrop=true;legacyBackdrop=active;
        if(active){
            // Hiding the Web host must reveal WC_VIEW's configured desktop,
            // never the legacy lobby/welcome controls underneath it.
            for(auto window:{lobby,welcome})if(visible(window)){
                rememberBackdropWindow(window);ShowWindow(window,SW_HIDE);
            }
        }else{
            std::vector<HWND> windows;windows.swap(backdropHiddenWindows);
            for(auto window:windows)if(IsWindow(window))
                SetWindowPos(window,host,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        }
        updatingLegacyBackdrop=false;
    }
    void raiseCertificates(){
        certificates.erase(std::remove_if(certificates.begin(),certificates.end(),[](const Certificate& c){return !IsWindow(c.window);}),certificates.end());
        bool active=certificateDismissPending;
        for(auto& certificate:certificates){
            const auto window=certificate.window;
            if(visible(window))certificate.shown=true;
            // Creation and the first native ShowWindow can be separate
            // messages. Keep the lobby hidden during that interval too.
            if(!certificate.shown||visible(window))active=true;
            if(!visible(window))continue;
            // Native certificates are root/view children. Raising the actual
            // native window preserves its owner, OK handler and close flow.
            if(GetWindow(window,GW_HWNDPREV))SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
        nativeCertificateActive=active;
        const bool block=active;
        setBackgroundOnly(block);
        if(certificateInputBlocked!=block){certificateInputBlocked=block;EnableWindow(host,block?FALSE:TRUE);}
    }
    void bounds(){
        if(!IsWindow(view))return;
        RECT r{};if(!GetClientRect(view,&r))return;
        raiseCertificates();
        const bool resized=!hostBoundsValid||!EqualRect(&hostBounds,&r);
        const bool raise=GetWindow(host,GW_HWNDPREV)&&!certificateInputBlocked;
        if(resized||raise){
            UINT flags=SWP_NOACTIVATE;
            if(!resized)flags|=SWP_NOMOVE|SWP_NOSIZE;
            if(!raise)flags|=SWP_NOZORDER;
            if(SetWindowPos(host,HWND_TOP,0,0,r.right,r.bottom,flags)){hostBounds=r;hostBoundsValid=true;}
        }
        if(controller){
            if((!controllerBoundsValid||!EqualRect(&controllerBounds,&r))&&SUCCEEDED(controller->put_Bounds(r))){controllerBounds=r;controllerBoundsValid=true;}
            POINT position{};
            if(ClientToScreen(view,&position)&&(!parentPositionValid||position.x!=parentPosition.x||position.y!=parentPosition.y)){
                if(SUCCEEDED(controller->NotifyParentWindowPositionChanged())){parentPosition=position;parentPositionValid=true;}
            }
        }
    }
    void show() {
        if(closing||updatingLegacyBackdrop)return;
        updateSettingsMenu();
        raiseCertificates();
        if(!lobby||!IsWindow(lobby))lobby=GetDlgItem(view,3009);
        // The original Windows view owns the entire game/welcome lifecycle.
        // Keep the original table's geometry and drawing. When the cover leaves,
        // invalidate the entire view, including native buttons outside WC_GAME.
        bool game=false;EnumChildWindows(view,findGame,reinterpret_cast<LPARAM>(&game));
        if(!game)gamePainted=false;
        if(game&&gamePainted&&!startupReady){
            startupReady=true;diagnostic("Web lobby: native table first paint released startup cover.");
        }
        bool showLobby=!failed&&(gameReturn.returning()||!game||!gamePainted);
        // A deferred HELO is not itself a certificate. Keep the Web lobby up
        // for a known ordinary return, including native profile construction.
        const bool returnBackdrop=gameReturn.returning()&&
            ((!certificateRecord.empty()&&!certificateDelivered)||
             (gameReturn.pending()&&!returnProfileReceived&&!gameReturn.stopped()));
        if((gameReturn.waitingForOK()&&game)||returnBackdrop)showLobby=false;
        // NINTEI is a native root/view child. Uncover the original WC_VIEW
        // backdrop for its entire lifetime, including before its first ShowWindow.
        // Leaving the Web host up while waiting for a hidden controller's
        // animation frame covers that backdrop with the host's black brush.
        if(nativeCertificateActive||nativeSettingsActive)showLobby=false;
        const bool backdrop=!failed&&(returnBackdrop||nativeCertificateActive||nativeSettingsActive);
        if(backdrop)updateLegacyBackdrop(true);
        if(showLobby)bounds();
        const bool showController=showLobby&&(!startupReady||(ready&&rendered));
        if(controller&&controllerVisible!=showController&&SUCCEEDED(controller->put_IsVisible(showController?TRUE:FALSE)))controllerVisible=showController;
        const bool wasCovered=(GetWindowLongPtrW(host,GWL_STYLE)&WS_VISIBLE)!=0;
        if(wasCovered!=showLobby)ShowWindow(host,showLobby?SW_SHOWNOACTIVATE:SW_HIDE);
        // Restore legacy visibility only after the Web host covers it again.
        if(!backdrop)updateLegacyBackdrop(false);
        if(wasCovered&&!showLobby&&!nativeRepaintPending){
            // Wait until the current native WM_PAINT has returned. Repainting
            // inside it would leave transparent controls using a stale backing.
            nativeRepaintPending=PostMessageW(view,repaintNativeMessage,0,0)!=FALSE;
        }
        raiseCertificates();
        updateStartupCover();
    }
    void repaintNative(){
        nativeRepaintPending=false;
        if(closing||(GetWindowLongPtrW(host,GWL_STYLE)&WS_VISIBLE))return;
        // WM_ERASEBKGND alone is not a completed first frame. A one-time redraw
        // after uncovering restores backgrounds behind the original buttons.
        RedrawWindow(view,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
    }
    void browse(const std::string& url){
        if(url.size()>4096||url.find_first_of("\r\n\t\"")!=url.npos||(url.rfind("https://",0)!=0&&url.rfind("http://",0)!=0)){error("请输入 HTTP(S) 浏览器链接。");return;}
        auto target=LobbyProtocol::Wide(url);ShellExecuteW(GetAncestor(view,GA_ROOT),L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
    void copyLink(const std::string& url){
        if(url.size()>4096||url.find_first_of("\r\n\t\"")!=url.npos||!std::regex_match(url,std::regex("https?://tenhou\\.net/[^\\s]*[?&](?:log|wg)=[^\\s]+")))return;
        auto value=LobbyProtocol::Wide(url);const auto bytes=(value.size()+1)*sizeof(wchar_t);
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!memory){error("无法复制链接，请重试。");return;}
        auto data=GlobalLock(memory);if(!data){GlobalFree(memory);error("无法复制链接，请重试。");return;}memcpy(data,value.c_str(),bytes);GlobalUnlock(memory);
        if(!OpenClipboard(GetAncestor(view,GA_ROOT))){GlobalFree(memory);error("无法复制链接，请重试。");return;}
        bool copied=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory);CloseClipboard();if(!copied){GlobalFree(memory);error("无法复制链接，请重试。");}
    }
    void open(const std::string& url) {
        std::string value=url;auto first=value.find_first_not_of(" \r\n\t"),last=value.find_last_not_of(" \r\n\t");if(first==value.npos)return;value=value.substr(first,last-first+1);
        if(value.size()>4096||value.find_first_of("\r\n\t\"")!=value.npos)return;
        const std::regex replay("^[0-9]{10}gm-[0-9a-fA-F]{4}-[0-9]{4}-[0-9a-fA-F]+$");
        if(std::regex_match(value,replay))value="https://tenhou.net/0/?log="+value;
        const bool tenhou=value.rfind("https://tenhou.net/",0)==0||value.rfind("http://tenhou.net/",0)==0;
        std::smatch match;
        if(tenhou&&std::regex_search(value,match,std::regex("[?&](log|wg)=([^&#]+)"))){std::string canonical="https://tenhou.net/0/?"+match[1].str()+"="+match[2].str();std::smatch seat;if(std::regex_search(value,seat,std::regex("[?&]tw=([0-3])(?:&|$)")))canonical+="&tw="+seat[1].str();value=canonical;}
        else {browse(value);return;}
        // Same private message used by the original embedded-browser links.
        SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>(value.c_str()));
    }
    void login(const std::string& payload){
        if(loggedIn){synchronize();return;}
        if(loginPending)return;
        auto split=payload.find('\n');auto id=payload.substr(0,split);
        if(id!="NoName"&&!std::regex_match(id,std::regex("ID[0-9A-Fa-f]{8}-[0-9A-Za-z]{8}"))){error("请输入有效的天凤 ID。");return;}
        WebTransport::SetActive(canResume(LobbyProtocol::Wide(id))&&WebTransport::Enabled());
        if(!IsWindow(welcome))welcome=GetDlgItem(view,3007);if(!classIs(welcome,L"WC_WELCOME")){error("Windows 登录窗口尚未就绪。");return;}
        if(connectionUnavailable()||!IsWindowEnabled(GetDlgItem(welcome,1))||canResume(LobbyProtocol::Wide(id))){
            reconnectLogin=payload;loginState(true);beginReconnect(Reconnect::Login);return;
        }
        submitLogin(payload);
    }
    void submitLogin(const std::string& payload){
        auto split=payload.find('\n');auto id=payload.substr(0,split);auto requested=split==payload.npos?"M":payload.substr(split+1);
        account=LobbyProtocol::Wide(id);SetWindowTextW(GetDlgItem(welcome,3002),account.c_str());
        if(requested!=sex){SendMessageA(welcome,WM_COMMAND,MAKEWPARAM(3003,0),reinterpret_cast<LPARAM>(GetDlgItem(welcome,3003)));sex=requested;}
        loginState(true);
        SendMessageA(welcome,WM_COMMAND,MAKEWPARAM(1,0),reinterpret_cast<LPARAM>(GetDlgItem(welcome,1)));
    }
    HWND selectTab(int index) {
        HWND tabs=GetDlgItem(lobby,4004);if(!tabs)tabs=GetDlgItem(lobby,4005);if(!tabs)return nullptr;
        SendMessageA(tabs,CB_SETCURSEL,index,0);
        SendMessageA(lobby,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(tabs),CBN_SELCHANGE),reinterpret_cast<LPARAM>(tabs));
        return GetDlgItem(lobby,4001);
    }
    void logs() {
        const auto messages=LobbyServices::ReadLocalReplays(directory);
        post(messages.history);post(messages.files);
    }
    bool hasNoMembership()const{
        for(const auto* tag:{"PROF","HELO"}){
            const auto event=events.find(tag);if(event==events.end())continue;
            const auto days=xmlAttribute(event->second,"expiredays");int remaining=0;
            if(LobbyProtocol::Integer(days,-1000000,1000000,remaining))return remaining<=0;
            if(event->second.find(" expire=\"")!=std::string::npos){
                const auto expiry=xmlAttribute(event->second,"expire");
                return expiry.empty()||expiry=="0";
            }
        }
        // The native profile parser initializes absent expiration attributes
        // to zero. A partial PROF must first inherit any known HELO expiry.
        return events.find("HELO")!=events.end()||events.find("PROF")!=events.end();
    }
    void join(int type) {
        if(canResume(account)){
            clearReservations();error("原对局尚未确认结束，请重新登录续局。",true);
            connectionTrace("JOIN blocked while the account still has an unfinished game");return;
        }
        if(gameReturn.stopped()){
            // Only an explicit reservation restarts an exhausted return. A
            // fresh connection must authenticate before releasing queuedJoin.
            WritePrivateProfileStringW(L"STATUS",L"GPID",nullptr,(directory+L"\\config.ini").c_str());
            WebTransport::SetActive(WebTransport::Enabled()&&(type&1));
            queuedJoin=type;beginReconnect(Reconnect::Lobby);postReturnState();return;
        }
        if(gameReturn.pending()){queuedJoin=type;return;}
        if(reconnect==Reconnect::Lobby||reconnect==Reconnect::Login){if(reconnect==Reconnect::Lobby)queuedJoin=type;return;}
        nativeViewer=false;
        if(!visible(lobby)){error("请返回大厅后预约。",true);return;}
        const bool useWeb=WebTransport::Enabled()&&(type&1);
        if(useWeb!=WebTransport::Active()){
            switchRoute(useWeb);queuedJoin=type;beginReconnect(Reconnect::Lobby);return;
        }
        if(connectionUnavailable()){queuedJoin=type;beginReconnect(Reconnect::Lobby);return;}
        const int index=LobbyProtocol::ReservationIndex(type);
        if(WebTransport::Enabled()&&(type&1)){
            // Web3's server applies rank/rating/membership requirements. The
            // native Windows buttons enforce a different client subscription.
            if(index<0){error("没有对应的预约项目。",true);return;}
            const auto reservation=std::to_string(lobbyId())+","+std::to_string(type);
            const auto xml="<JOIN t=\""+reservation+"\" />";
            if(nativeSend(xml.c_str())!=static_cast<int>(xml.size()+1)){clearReservations();error("Web版连接未就绪，预约未发送。",true);}
            else {webReservations.insert(reservation);diagnostic("Web lobby: Web reservation submitted.");}
            return;
        }
        if(type&1) {
            HWND main=selectTab(0),join=GetDlgItem(main,4007),levels=GetDlgItem(join,4006);
            if(index<0||!join||!levels){error("Windows 客户端没有对应的预约项目。",true);return;}
            const int level=((type&128)?1:0)|((type&32)?2:0);
            SendMessageA(levels,CB_SETCURSEL,level,0);
            SendMessageA(join,WM_COMMAND,MAKEWPARAM(4006,CBN_SELCHANGE),reinterpret_cast<LPARAM>(levels));
            RedrawWindow(join,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW|RDW_ALLCHILDREN);
            HWND button=GetDlgItem(join,4015+index);
            if(!button||!IsWindowEnabled(button)||!visible(button)||visible(GetDlgItem(main,4012))) {
                if(hasNoMembership()&&ShowNotice(GetAncestor(view,GA_ROOT),Notice::MembershipExpired))return;
                error("Windows 客户端当前不允许匹配，请检查有料会员期限及入场条件。",true,"matchUnavailable");return;
            }
            SendMessageA(join,WM_COMMAND,MAKEWPARAM(4015+index,0),reinterpret_cast<LPARAM>(button));
        } else if(type==0||type==16||type==64||type==1536||type==1552) {
            std::string xml="<JOIN t=\""+std::to_string(lobbyId())+","+std::to_string(type)+"\" />";nativeSend(xml.c_str());
        } else error("Windows 客户端没有对应的试玩模式。",true);
    }
    void command(const std::wstring& message) {
        auto value=LobbyProtocol::Utf8(message);if(value.size()>8192)return;auto split=value.find('\n');if(split==value.npos)return;
        const auto name=value.substr(0,split),payload=value.substr(split+1);int number=0;
        if(name=="ready"){
            ready=true;diagnostic("Web lobby: renderer bridge ready.");synchronize();
            if(!startupReady)post("{\"type\":\"startup\",\"sequence\":"+std::to_string(++startupSequence)+"}");
            show();return;
        }
        if(name=="rendered"){if(!rendered)diagnostic("Web lobby: 4K page rendered.");rendered=true;show();return;}
        if(name=="startupRendered"&&LobbyProtocol::Integer(payload,1,2147483647,number)){
            if(!startupReady&&static_cast<unsigned>(number)==startupSequence){
                startupReady=true;diagnostic("Web lobby: login assets and first paint ready.");show();
            }
            return;
        }
        if(!ready||!nativeSend)return;
        if(backgroundOnly&&name!="options"&&name!="logs"&&name!="watch"&&name!="watchFilters"&&name!="accountLogs")return;
        // Do not start a viewer, change room or emit lobby commands while the
        // server is still transferring the finished game's identity.
        if(gameReturn.pending()&&(name=="open"||name=="room"||name=="rank"||name=="chat"||name=="login"))return;
        if(name=="join"&&LobbyProtocol::Integer(payload,0,32767,number))join(number);
        else if(name=="cancel"){
            if(gameReturn.pending()){clearReservations();return;}
            if(WebTransport::Active()){
                // Retire the reservation socket, then restore the ordinary
                // native lobby through its existing login/profile pipeline.
                returnToNative();return;
            }
            const bool deferred=gameReturn.pending()||reconnect!=Reconnect::None||queuedJoin>=0||connectionUnavailable();
            clearReservations();
            if(deferred)return;
            SendMessageA(lobby,WM_COMMAND,MAKEWPARAM(2,0),reinterpret_cast<LPARAM>(GetDlgItem(lobby,2)));
        }
        else if(name=="rank"&&LobbyProtocol::Integer(payload,-1,32767,number)){const auto xml="<PXR V=\""+std::to_string(number)+"\" />";nativeSend(xml.c_str());}
        else if(name=="open")open(payload);
        else if(name=="copy")copyLink(payload);
        else if(name=="browser")browse(payload);
        else if(name=="login")login(payload);
        else if(name=="register"&&!loggedIn){
            auto first=payload.find('\n'),second=first==payload.npos?first:payload.find('\n',first+1);int sequence=0;
            if(first==payload.npos||second==payload.npos||!LobbyProtocol::Integer(payload.substr(0,first),1,1000000,sequence))return;
            auto action=payload.substr(first+1,second-first-1),nameValue=payload.substr(second+1);auto wide=LobbyProtocol::Wide(nameValue);
            if((action!="query"&&action!="create")||wide.empty()||wide.size()>8||nameValue.find_first_of("\r\n\t")!=nameValue.npos){error("请输入 8 个字以内的昵称。");return;}
            if(registration.valid()){error("正在处理注册请求，请稍候。");return;}
            registration=std::async(std::launch::async,LobbyServices::RegisterName,sequence,action=="query",nameValue);
        }
        else if(name=="room")room(payload);
        else if(name=="options")options();
        else if(name=="option")option(payload);
        else if(name=="volume"&&LobbyProtocol::Integer(payload,0,10,number)){
            const int nativeVolume=number*20;
            WritePrivateProfileStringW(L"RESOURCE",L"SE_VOL",std::to_wstring(nativeVolume).c_str(),(directory+L"\\config.ini").c_str());
            using SetVolume=void(__cdecl*)(double);reinterpret_cast<SetVolume>(reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr))+0x6e450)(nativeVolume/100.0);
        }
        else if(name=="fullscreen"){HWND root=GetAncestor(view,GA_ROOT);SendMessageW(root,WM_SYSCOMMAND,IsZoomed(root)?SC_RESTORE:SC_MAXIMIZE,0);}
        else if(name=="logout"){
            clearReservations();
            if(reconnect!=Reconnect::None){auto network=GetDlgItem(GetAncestor(view,GA_ROOT),3006);if(classIs(network,L"WC_CLIENT"))DestroyWindow(network);}
            clearReconnect();gameReturn.reset();clearReturnProfile();queuedJoin=-1;loggedIn=false;events.clear();loginState(false);sendWelcome();
            if(!connectionUnavailable())nativeSend("<BYE />");
            SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("GOWELCOME"));show();
        }
        else if(name=="subscription"){auto id=LobbyProtocol::Utf8(account);browse("https://tenhou.net/reg/"+(id.size()==19&&id.rfind("ID",0)==0?"?"+id.substr(0,10):""));}
        else if(name=="settings")SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("OPENCONF=0"));
        else if(name=="logs")logs();
        else if(name=="accountLogs"&&LobbyProtocol::Integer(payload,1,2147483647,number))requestAccountLogs(number);
        else if(name=="watch"&&loggedIn&&LobbyProtocol::Integer(payload,1,2147483647,number))requestWatch(number);
        else if(name=="watchFilters"&&LobbyProtocol::Integer(payload,0,127,number)){
            number=static_cast<int>(LobbyProtocol::NormalizeWatchFlags(number));
            // Native and /4 use the same seven filter bits. Keep the old
            // controls synchronized for returning to a native-only fallback.
            auto ini=directory+L"\\config.ini";if(GetPrivateProfileIntW(L"STATUS",L"LOBBY_WG_FLAGS",defaultWatchFlags,ini.c_str())!=static_cast<UINT>(number))WritePrivateProfileStringW(L"STATUS",L"LOBBY_WG_FLAGS",std::to_wstring(number).c_str(),ini.c_str());
            HWND main=GetDlgItem(lobby,4001),watch=GetDlgItem(main,4010);for(int bit=0;bit<7;++bit)if(HWND button=GetDlgItem(watch,4033+bit))SendMessageW(button,BM_SETCHECK,(number&(1<<bit))?BST_CHECKED:BST_UNCHECKED,0);
        } else if(name=="chat"&&!payload.empty()) {
            // URL-encode all bytes; the native transport receives one XML tag.
            const auto encoded=LobbyProtocol::UrlEncode(payload);
            const auto xml="<CHAT text=\""+encoded+"\" />";nativeSend(xml.c_str());
        }
    }
    void failInitialization(const char* message){
        if(closing||failed)return;
        failed=true;ready=false;loginPending=false;
        clearReconnect();gameReturn.reset();clearReturnProfile();queuedJoin=-1;
        if(controller)controller->Close();
        browser.Reset();controller.Reset();controllerVisible=false;
        show();diagnostic(message);
        if(LobbyConnection::FallbackToNative()){
            // The Web connection may already be open while WebView2 starts.
            // Destroy it through native CONNECT so both the fallback lobby
            // and its reservation controls use the original Windows service.
            disconnected=false;loggedIn=false;events.clear();
            SendMessageA(GetAncestor(view,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("CONNECT"));
        }
    }
    void initialize() {
        if(closing||failed||browser||controller||!loader)return;
        auto self=shared_from_this();
        using CreateEnvironment=HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
        auto create=reinterpret_cast<CreateEnvironment>(GetProcAddress(loader,"CreateCoreWebView2EnvironmentWithOptions"));
        if(!create){failInitialization("Web lobby: WebView2 loader export unavailable; original lobby retained.");return;}
        auto callback=Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([self](HRESULT result,ICoreWebView2Environment* environment)->HRESULT {
            if(self->closing||self->failed)return S_OK;
            if(FAILED(result)||!environment){self->failInitialization("Web lobby: runtime initialization failed; original lobby retained.");return S_OK;}
            const auto controllerResult=environment->CreateCoreWebView2Controller(self->host,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([self](HRESULT status,ICoreWebView2Controller* controller)->HRESULT {
                if(self->closing||self->failed)return S_OK;
                if(FAILED(status)||!controller){self->failInitialization("Web lobby: controller creation failed; original lobby retained.");return S_OK;}
                self->controller=controller;
                if(FAILED(controller->get_CoreWebView2(&self->browser))||!self->browser){self->failInitialization("Web lobby: browser creation failed; original lobby retained.");return S_OK;}
                controller->put_IsVisible(FALSE);
                ComPtr<ICoreWebView2Controller2> background;if(SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&background))))background->put_DefaultBackgroundColor({255,0,0,0});
                self->bounds();
                ComPtr<ICoreWebView2_3> mapping;if(FAILED(self->browser.As(&mapping))||FAILED(mapping->SetVirtualHostNameToFolderMapping(L"tenhou.local",(directory+L"\\web").c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {self->failInitialization("Web lobby: local resource mapping failed; original lobby retained.");return S_OK;}
                ComPtr<ICoreWebView2Settings> settings;
                if(FAILED(self->browser->get_Settings(&settings))||!settings){self->failInitialization("Web lobby: browser settings unavailable; original lobby retained.");return S_OK;}
                settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_IsStatusBarEnabled(FALSE);settings->put_AreDevToolsEnabled(FALSE);
                EventRegistrationToken token{};
                self->browser->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([self](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT {
                    BOOL success=FALSE;args->get_IsSuccess(&success);
                    if(!success&&!self->rendered)self->failInitialization("Web lobby: local page navigation failed; original lobby retained.");
                    return S_OK;
                }).Get(),&token);
                self->browser->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([self](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
                    LPWSTR source=nullptr,message=nullptr;args->get_Source(&source);
                    const bool trusted=source&&std::wstring_view(source).rfind(origin,0)==0;CoTaskMemFree(source);
                    if(trusted&&!self->closing&&SUCCEEDED(args->TryGetWebMessageAsString(&message))&&message){self->commands.push_back(message);PostMessageW(self->view,commandMessage,0,0);}CoTaskMemFree(message);return S_OK;
                }).Get(),&token);
                self->browser->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([self](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT {
                    LPWSTR uri=nullptr;args->get_Uri(&uri);if(uri&&std::wstring_view(uri).rfind(origin,0)!=0){args->put_Cancel(TRUE);self->open(LobbyProtocol::Utf8(uri));}CoTaskMemFree(uri);return S_OK;
                }).Get(),&token);
                self->browser->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([self](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT {
                    LPWSTR uri=nullptr;args->get_Uri(&uri);args->put_Handled(TRUE);
                    if(uri){auto url=LobbyProtocol::Utf8(uri);BOOL user=FALSE;args->get_IsUserInitiated(&user);
                        if(url.rfind("https://tenhou.net/",0)==0||url.rfind("http://tenhou.net/",0)==0)self->open(url);
                        else if(user&&url.rfind("https://",0)==0&&url.find_first_of("\r\n\t\"")==url.npos)ShellExecuteW(GetAncestor(self->view,GA_ROOT),L"open",uri,nullptr,nullptr,SW_SHOWNORMAL);
                    }CoTaskMemFree(uri);return S_OK;
                }).Get(),&token);
                if(FAILED(self->browser->Navigate(L"https://tenhou.local/index.html"))){self->failInitialization("Web lobby: local page navigation request failed; original lobby retained.");return S_OK;}
                self->show();diagnostic("Web lobby: local 4K renderer initialized; native table retained.");return S_OK;
            }).Get());
            if(FAILED(controllerResult))self->failInitialization("Web lobby: controller request failed; original lobby retained.");
            return S_OK;
        });
        HRESULT result=create(nullptr,(directory+L"\\cache\\webview2").c_str(),nullptr,callback.Get());
        if(FAILED(result))failInitialization("Web lobby: environment request failed; original lobby retained.");
    }
};
std::unordered_map<HWND,std::shared_ptr<Session>> sessions;
std::shared_ptr<Session> owner(HWND w){for(HWND parent=w;parent;parent=GetParent(parent)){auto it=sessions.find(parent);if(it!=sessions.end())return it->second;}return {};}
std::shared_ptr<Session> certificateSession(HWND w){
    if(auto current=owner(w))return current;
    // Certificates belong to the root, alongside WC_VIEW.
    const auto root=GetAncestor(w,GA_ROOT);
    for(auto& entry:sessions)if(!entry.second->closing&&GetAncestor(entry.first,GA_ROOT)==root)return entry.second;
    return {};
}
LRESULT CALLBACK certificateObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    auto found=sessions.find(reinterpret_cast<HWND>(data));
    if(found==sessions.end())return DefSubclassProc(w,msg,wp,lp);
    auto current=found->second;
    // Keep the backdrop through native hide/destruction callbacks. Reentrant
    // show() calls must not expose the lobby halfway through certificate OK.
    if((msg==WM_SHOWWINDOW&&!wp)||msg==WM_DESTROY||msg==WM_NCDESTROY)
        current->certificateDismissPending=true;
    if(msg==WM_WINDOWPOSCHANGING&&lp){
        auto position=reinterpret_cast<WINDOWPOS*>(lp);
        if(!(position->flags&SWP_HIDEWINDOW)){position->hwndInsertAfter=HWND_TOP;position->flags&=~SWP_NOZORDER;}
    }
    if(msg==WM_SHOWWINDOW&&wp)for(auto& certificate:current->certificates)if(certificate.window==w)certificate.shown=true;
    if(msg==WM_NCDESTROY){
        auto& windows=current->certificates;windows.erase(std::remove_if(windows.begin(),windows.end(),[w](const Session::Certificate& c){return c.window==w;}),windows.end());
        RemoveWindowSubclass(w,certificateObserver,id);
    }
    auto result=DefSubclassProc(w,msg,wp,lp);
    if(msg==WM_SHOWWINDOW||msg==WM_WINDOWPOSCHANGED||msg==WM_NCDESTROY)PostMessageW(current->view,nativeCertificateChangedMessage,0,0);
    return result;
}
void attachCertificate(HWND w){
    auto current=certificateSession(w);
    if(!current)return;
    if(!SetWindowSubclass(w,certificateObserver,viewSubclass,reinterpret_cast<DWORD_PTR>(current->view)))return;
    current->certificates.push_back({w,visible(w)});
    current->certificateDelivered=true;
    // Native WM_CREATE posts the certificate's initial layout/paint. Remove
    // the Web cover now, before that queued work snapshots its background.
    current->show();
    PostMessageW(current->view,nativeCertificateChangedMessage,0,0);
    diagnostic("Web lobby: native rank certificate uses original backdrop until dismissal.");
}
std::shared_ptr<Session> threadSession(){for(auto& entry:sessions)if(!entry.second->closing&&entry.second->uiThread==GetCurrentThreadId())return entry.second;return {};}
int __cdecl leaveGame(const char* xml){
    if(auto current=threadSession())current->requestReturn();
    return sendGameReturn(xml);
}
BOOL WINAPI postResultReturn(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    // This exact native call is the final result's OK (WM_COMMAND 4001),
    // not a socket reconnect's automatically posted GOLOBBY.
    if(auto current=threadSession())current->requestReturn();
    return PostMessageA(w,msg,wp,lp);
}
BOOL WINAPI postGameConnectionClose(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    // Native owari posts WM_CLOSE to WC_CLIENT (00492F67), before result OK.
    // Deliver BYE on that still-live game socket, before its destructor closes
    // it. Waiting until OK sends nothing and leaves post-game HELO rejected.
    if(auto current=threadSession();current&&current->gameReturn.needsFinalBye()&&
       msg==WM_CLOSE&&w==GetDlgItem(GetAncestor(current->view,GA_ROOT),3006)&&nativeSocket&&*nativeSocket!=MAXDWORD){
        constexpr char bye[]="<BYE />";
        if(nativeSend(bye)==static_cast<int>(sizeof(bye))){
            current->gameReturn.sent(bye);returnTrace("final BYE delivered before native game connection close");
        }else returnTrace("final BYE could not be sent before native game connection close");
    }
    return PostMessageA(w,msg,wp,lp);
}
int __cdecl sendGameReturn(const char* xml){
    if(!xml)return nativeSend(xml);
    const auto length=strnlen_s(xml,8193);if(length>8192)return nativeSend(xml);
    for(auto& entry:sessions){
        auto& session=*entry.second;if(session.closing||session.uiThread!=GetCurrentThreadId())continue;
        auto packet=session.gameReturn.prepare(std::string_view(xml,length));
        if(packet.empty()){returnTrace("duplicate BYE/HELO ignored");return static_cast<int>(length+1);}
        if(session.gameReturn.pending()&&nativeSocket&&*nativeSocket==MAXDWORD){session.gameReturn.disconnected();return -1;}
        if(LobbyProtocol::Tag(packet)=="HELO"&&session.gameReturn.needsBye()){
            // /4 leaves the game before its return HELO. Some native paths
            // reach GOLOBBY directly, so complete that missing half first.
            constexpr char bye[]="<BYE />";
            const int sent=nativeSend(bye);if(sent!=static_cast<int>(sizeof(bye))){session.gameReturn.stop();std::fill(packet.begin(),packet.end(),'\0');return sent;}
            session.gameReturn.sent(bye);
        }
        const auto tag=LobbyProtocol::Tag(packet);
        if(tag=="HELO"){
            // Automatic native resume may send HELO before WC_WELCOME ever
            // exists. Bind the session to the account actually being sent.
            const auto id=xmlAttribute(packet,"name");
            if(id=="NoName"||std::regex_match(id,std::regex("ID[0-9A-Fa-f]{8}-[0-9A-Za-z]{8}")))session.account=LobbyProtocol::Wide(id);
        }
        const int sent=nativeSend(packet.c_str());
        if(tag=="HELO"||tag=="BYE"||tag=="GOK")connectionTrace(tag=="HELO"?"native HELO sent":tag=="BYE"?"native BYE sent":"native GOK sent");
        if(tag=="HELO"&&session.reconnect==Session::Reconnect::Login&&session.reconnectHelloSent)
            session.reconnectPacketSent=sent==static_cast<int>(packet.size()+1);
        if(sent==static_cast<int>(packet.size()+1))session.gameReturn.sent(packet);
        else session.gameReturn.stop();
        std::fill(packet.begin(),packet.end(),'\0');return sent;
    }
    return nativeSend(xml);
}
LRESULT CALLBACK coverObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    auto found=sessions.find(reinterpret_cast<HWND>(data));
    auto current=found!=sessions.end()?found->second:owner(w);
    if(msg==WM_ERASEBKGND&&wp){if(current)current->paintCover(w,reinterpret_cast<HDC>(wp));return TRUE;}
    if(msg==WM_PAINT){PAINTSTRUCT paint{};auto dc=BeginPaint(w,&paint);if(current)current->paintCover(w,dc);EndPaint(w,&paint);return 0;}
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(w,coverObserver,id);
    return DefSubclassProc(w,msg,wp,lp);
}
LRESULT CALLBACK loadingObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    auto found=sessions.find(reinterpret_cast<HWND>(data));
    if(found!=sessions.end()&&visible(w)&&
       ((msg==WM_WINDOWPOSCHANGING&&lp&&(reinterpret_cast<WINDOWPOS*>(lp)->flags&SWP_HIDEWINDOW))||msg==WM_DESTROY))
        found->second->captureStartupFrame();
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(w,loadingObserver,id);
    return DefSubclassProc(w,msg,wp,lp);
}
void attachLoading(HWND w){
    const auto root=GetAncestor(w,GA_ROOT);
    for(auto& entry:sessions)if(!entry.second->closing&&GetAncestor(entry.first,GA_ROOT)==root){
        SetWindowSubclass(w,loadingObserver,viewSubclass,reinterpret_cast<DWORD_PTR>(entry.first));break;
    }
}
LRESULT CALLBACK rootObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    auto it=sessions.find(reinterpret_cast<HWND>(data));if(it==sessions.end())return DefSubclassProc(w,msg,wp,lp);auto current=it->second;
    bool nativeUrl=false,returnProfile=false,prefetchedProfile=false;unsigned nativeTicket=0;
    if(current->retryLoginQueued&&(msg==0x8201||msg==0x8202||msg==0x8209))return 0;
    if(msg==0x8202&&wp==1004&&current->reconnect==Session::Reconnect::Abandoned&&current->canResume(current->account))return 0;
    if(msg==WM_INITMENUPOPUP&&HIWORD(lp))current->updateSettingsMenu();
    if(msg==WM_SYSCOMMAND&&(wp&0xfff0)==nativeSettingsCommand){
        current->updateSettingsMenu();
        if(!current->nativeSettingsBlocked()){
            current->commands.push_back(L"settings\n");PostMessageW(current->view,commandMessage,0,0);
        }
        return 0;
    }
    if(msg==0x8201&&current->gameReturn.completedGame()){
        const auto event=LOWORD(lp),error=HIWORD(lp);
        if(event==16||event==32){char line[80];sprintf_s(line,"socket event=%u error=%u",event,error);returnTrace(line);}
        if(event==32){current->disconnected=true;current->gameReturn.disconnected();return 0;}
        if(event==16&&current->gameReturn.pending()){
            if(error){current->gameReturn.stop();return 0;}
            current->gameReturn.connected();
            if(current->gameReturn.prefetching()&&current->gameReturn.waitingForOK()){
                nativeSend("<Z />");SendMessageA(w,0x8204,0,reinterpret_cast<LPARAM>("GOLOBBY"));return 0;
            }
        }
    }
    if(msg==0x8201){
        const auto event=LOWORD(lp),failure=HIWORD(lp);
        if(event==16||event==32)connectionTrace(event==16?"FD_CONNECT":"FD_CLOSE",failure);
        // A timed-out attempt may still finish asynchronously. Only a new
        // explicit action may replace it; never auto-login on that late event.
        if(current->reconnect==Session::Reconnect::Abandoned&&(event==16||event==32))return 0;
        if(event==32){
            current->awaitingTablePaint=false;
            current->disconnected=true;current->queuedJoin=-1;
            if(current->reconnect!=Session::Reconnect::None){current->failReconnect();return 0;}
            // The network window has already retired nativeSocket. Intercept
            // the root's UI branch too: the Web lobby shows its own notice
            // instead of the native CONNECTION CLOSED MessageBox.
            if(ShowNotice(w,Notice::ConnectionClosed)){
                current->gameReturn.reset();current->clearReturnProfile();current->loginState(false);
                diagnostic("Web lobby: disconnected; native exit UI suppressed.");return 0;
            }
        }
        if(event==16&&current->reconnect!=Session::Reconnect::None){
            if(failure){current->failReconnect();return 0;}
            if(current->reconnect==Session::Reconnect::Login){
                // Native FD_CONNECT would navigate to GOLOBBY/GOWELCOME and
                // may auto-submit stale welcome controls. Submit only the ID
                // selected by this explicit login, after the socket is ready.
                nativeSend("<Z />");current->sendReconnectLogin();return 0;
            }
            // Lobby recovery keeps native GOLOBBY/HELO handling. The queued
            // reservation is released only when observe receives its profile.
        }
    }
    // ERR XML normally posts 0x8202, which displays the modal and then posts
    // GOWELCOMEBYERR. Keep a live-game recovery in one bounded operation.
    if(msg==0x8202&&wp==1004&&(current->retryGameResume("ERR 1004; waiting for old connection to retire")||current->retryRouteSwitch()||current->gameReturn.busyError()))return 0;
    if(msg==0x8202&&wp==1020&&ShowNotice(w,Notice::MembershipExpired)){current->clearReservations();return 0;}
    if(msg==0x8202&&wp==1024&&ShowNotice(w,Notice::EligibilityRejected)){current->clearReservations();return 0;}
    if(msg==0x8209&&lp){const auto xml=reinterpret_cast<const char*>(lp);size_t n=strnlen_s(xml,1024*1024+1);if(n<=1024*1024){
        const std::string_view event(xml,n);const auto tag=LobbyProtocol::Tag(event);
        if(current->reconnect==Session::Reconnect::Abandoned)return 0;
        if(current->recoveryProfile(event))return 0;
        if(tag=="HELO"&&current->gameReturn.prepared()){
            returnProfile=true;
            prefetchedProfile=returnPrefetchHooks&&!current->returnNavigationReady;
            if(prefetchedProfile)current->retainReturnProfile(event);
            else current->returnProfileReceived=true;
        }
        if((tag=="HELO"||tag=="PROF")&&(current->gameReturn.waitingForOK()||current->gameReturn.pending()))
            returnTrace(xmlAttribute(event,"nintei").empty()?"return profile has no rank certificate":"return profile includes rank certificate");
        if(tag=="GO"||tag=="REINIT"||tag=="TAIKYOKU"||tag=="SAIKAI")connectionTrace(tag=="GO"?"GO received":tag=="REINIT"?"REINIT received":tag=="TAIKYOKU"?"TAIKYOKU received":"SAIKAI received");
        if(tag=="REJOIN"&&current->canResume(current->account)){connectionTrace("REJOIN ignored for unfinished game");return 0;}
        if(tag=="REJOIN"&&current->rejoin(event))return 0;
        if(tag=="ERR"&&xmlAttribute(event,"code")=="1020"&&ShowNotice(w,Notice::MembershipExpired)){
            current->observeConnection(event);
            diagnostic("Web lobby: membership rejection kept authenticated lobby.");return 0;
        }
        if(tag=="ERR"&&xmlAttribute(event,"code")=="1024"&&ShowNotice(w,Notice::EligibilityRejected)){
            current->observeConnection(event);
            diagnostic("Web lobby: eligibility rejection kept authenticated lobby.");return 0;
        }
        if(tag=="ERR"&&xmlAttribute(event,"code")=="1004"&&(current->retryGameResume("ERR 1004; waiting for old connection to retire")||current->retryRouteSwitch()||current->gameReturn.busyError()))return 0;
        if(tag=="REHELO"&&(current->gameReturn.waitingForOK()||current->gameReturn.pending())){
            current->gameReturn.rehelo();returnTrace("REHELO deferred to controlled return recovery");return 0;
        }
        // Feed official socket records to the WebView before native controls
        // process them. No UI polling, native count vectors or second login.
        if(tag=="HELO"||tag=="PROF"||tag=="LN")current->observe(event);else current->observeConnection(event);
    }}
    if(msg==0x8204&&static_cast<UINT_PTR>(lp)>0xffff){const auto command=reinterpret_cast<const char*>(lp);
        const std::string_view value(command);
        if(value.rfind("OPENCONF=",0)==0){
            // This native path sends BYE and later navigates to the lobby.
            // Recheck here: a queued click may predate GO or resume recovery.
            current->refreshTrialTable();
            if(current->nativeSettingsBlocked()){current->updateSettingsMenu();return 0;}
            // Native OPENCONF creates a preview WC_GAME and runs the modal
            // settings sheet. Keep that live table exposed through its nested
            // message loop, including timer-driven show() and preview paints.
            const bool previous=current->nativeSettingsActive;
            current->nativeSettingsActive=true;
            current->show();
            const auto result=DefSubclassProc(w,msg,wp,lp);
            current->nativeSettingsActive=previous;
            current->show();
            return result;
        }
        if(current->retryLoginQueued&&(value=="GOLOBBY"||value=="GOWELCOME"||value=="GOWELCOMEBYERR"||value=="CONNECT"))return 0;
        if(value=="GOLOBBY"||value=="GOWELCOME"||value=="GOWELCOMEBYERR"||value=="CONNECT"){
            if(current->gameReturn.completedGame())returnTrace(command);
            const bool preparing=current->gameReturn.prefetching()&&(value=="GOLOBBY"||(value=="CONNECT"&&current->allowReturnConnect));
            if(current->gameReturn.waitingForOK()&&!preparing){returnTrace("automatic navigation held until result OK");return 0;}
            if(current->gameReturn.returning()&&current->loggedIn&&(value=="GOWELCOME"||value=="GOWELCOMEBYERR")){
                returnTrace("incidental welcome ignored after completed game");current->synchronize();current->show();return 0;
            }
            const bool disconnectedReturn=value=="CONNECT"&&current->gameReturn.pending()&&nativeSocket&&*nativeSocket==MAXDWORD;
            if(disconnectedReturn)current->gameReturn.reconnecting();
            if(current->gameReturn.pending()&&value!="GOLOBBY"&&!(value=="CONNECT"&&(current->allowReturnConnect||disconnectedReturn))){
                returnTrace("automatic reconnect/login fallback held during handoff");return 0;
            }
        }
        nativeUrl=(value.rfind("https://tenhou.net/0/?",0)==0||value.rfind("http://tenhou.net/0/?",0)==0)&&(value.find("?wg=")!=value.npos||value.find("?log=")!=value.npos);
        if(nativeUrl){
            // /wg and replay navigation belong to the Windows client. Never
            // send their legacy commands over the Web matchmaking socket.
            // Authenticate the replacement TCP connection before replaying
            // this URL, outside the profile callback that builds native UI.
            if(WebTransport::Active()){
                current->returnToNative(value);return 0;
            }
            if(current->reconnect==Session::Reconnect::Login||current->reconnect==Session::Reconnect::Lobby){
                current->pendingViewerUrl.assign(value);current->nativeViewer=true;current->clearReservations();return 0;
            }
            current->nativeViewer=true;current->trialTable=false;current->gameReturn.reset();current->clearReturnProfile();current->queuedJoin=-1;
            // Native URL handling caches the URL and posts OPENURL to itself.
            // That continuation must reach the original window procedure.
            nativeTicket=++current->nativeOpenSequence;current->pendingNativeOpens.push_back(nativeTicket);
        }
        if(!strcmp(command,"GOWELCOME")||!strcmp(command,"GOWELCOMEBYERR")){current->pendingViewerUrl.clear();current->trialTable=false;current->clearReconnect();current->gameReturn.reset();current->clearReturnProfile();current->queuedJoin=-1;current->loggedIn=false;current->loginState(false);current->events.clear();current->sendWelcome();current->show();}
        else if(!strcmp(command,"GOLOBBY")){
            if(current->nativeViewer&&current->pendingViewerUrl.empty()&&current->reconnect==Session::Reconnect::None&&current->connectionUnavailable()){
                current->returnToNative();return 0;
            }
            if(WebTransport::Active()&&current->gameReturn.returning()){
                WebTransport::SetActive(false);current->gameReturn.reconnecting();current->allowReturnConnect=true;
                SendMessageA(w,0x8204,0,reinterpret_cast<LPARAM>("CONNECT"));
                current->allowReturnConnect=false;return 0;
            }
            if(current->gameReturn.returning()){
                current->returnNavigationReady=true;
                PostMessageW(current->view,finishReturnMessage,0,0);
            }
            if(current->gameReturn.duplicateReturn()){returnTrace("duplicate GOLOBBY ignored");return 0;}
            // Reuse the last authenticated profile, including a final PROF
            // delivered to WC_GAME, before exposing the WebView again.
            if(!current->gameReturn.waitingForOK()){
                if(current->loggedIn)current->synchronize();
                current->post("{\"type\":\"returnLobby\"}");
                current->postReturnState();current->show();
            }
        }
        else if(!strcmp(command,"OPENURL")){
            if(!current->pendingNativeOpens.empty())current->pendingNativeOpens.pop_front();
            else if(current->ready&&!current->gamePainted){current->post("{\"type\":\"openPrompt\"}");return 0;}
        }
    }
    const bool previousDefer=deferNativeProfile;
    if(returnProfile)deferNativeProfile=prefetchedProfile;
    // Decide the backdrop before native HELO creates or paints any windows.
    if(returnProfile&&!prefetchedProfile)current->show();
    auto result=DefSubclassProc(w,msg,wp,lp);
    if(returnProfile)deferNativeProfile=previousDefer;
    if(returnProfile&&!prefetchedProfile)current->show();
    if(msg==0x8209)current->queueCertificate();
    if(msg==WM_SIZE||msg==WM_WINDOWPOSCHANGED)current->updateStartupCover();
    if(nativeUrl)PostMessageW(current->view,nativeOpenFinishedMessage,nativeTicket,0);
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(w,rootObserver,id);return result;
}
LRESULT CALLBACK gameObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    auto current=owner(w);
    if(current&&msg==0x8209&&lp){const auto xml=reinterpret_cast<const char*>(lp);const auto length=strnlen_s(xml,1024*1024+1);if(length<=1024*1024)current->observe(std::string_view(xml,length));}
    if(current&&msg==WM_WINDOWPOSCHANGING&&lp&&(reinterpret_cast<WINDOWPOS*>(lp)->flags&SWP_HIDEWINDOW)){current->gamePainted=false;current->show();}
    if(current&&msg==WM_NCDESTROY){current->gamePainted=false;current->show();RemoveWindowSubclass(w,gameObserver,id);}
    auto result=DefSubclassProc(w,msg,wp,lp);
    if(current&&msg==WM_PAINT&&visible(w)){
        current->gamePainted=true;current->show();
        if(current->awaitingTablePaint){
            current->awaitingTablePaint=false;
            if(current->loginPending)current->loginState(false);
            current->post("{\"type\":\"gameStarted\"}");
        }
    }
    return result;
}
LRESULT CALLBACK lobbyObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    auto current=owner(w);
    if(current&&msg==WM_WINDOWPOSCHANGING&&lp&&!current->failed){
        auto pos=reinterpret_cast<WINDOWPOS*>(lp);pos->hwndInsertAfter=current->host;pos->flags&=~SWP_NOZORDER;
        if(current->legacyBackdrop&&(pos->flags&SWP_SHOWWINDOW)){
            current->rememberBackdropWindow(w);pos->flags=(pos->flags&~SWP_SHOWWINDOW)|SWP_HIDEWINDOW;
        }
    }
    if(current&&msg==0x8209&&lp){
        const auto xml=reinterpret_cast<const char*>(lp);const auto length=strnlen_s(xml,1024*1024+1);
        if(length<=1024*1024)current->observe(std::string_view(xml,length));
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(w,lobbyObserver,id);
    auto result=DefSubclassProc(w,msg,wp,lp);
    if(current&&msg==0x8209)current->queueCertificate();
    if(current&&(msg==WM_SHOWWINDOW||msg==0x8209))current->show();
    return result;
}
void attachLobby(HWND w) {
    auto it=sessions.find(GetParent(w));if(it==sessions.end())return;
    it->second->lobby=w;SetWindowSubclass(w,lobbyObserver,viewSubclass,0);
    SetWindowPos(w,it->second->host,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
void attachWelcome(HWND w){
    auto current=owner(w);if(!current)return;
    current->welcome=w;SetWindowSubclass(w,lobbyObserver,viewSubclass,0);
    if(current->gameReturn.completedGame()){returnTrace("incidental welcome creation retained completed-game identity");return;}
    current->rememberWelcome(w);
    if(current->reconnect==Session::Reconnect::Login||current->awaitingTablePaint){current->loginState(true);return;}
    current->gameReturn.reset();current->clearReturnProfile();current->queuedJoin=-1;current->loggedIn=false;current->loginState(false);
    SetWindowPos(w,current->host,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    current->sendWelcome();current->show();
}
LRESULT CALLBACK viewObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    auto it=sessions.find(w);if(it==sessions.end())return DefSubclassProc(w,msg,wp,lp);auto current=it->second;
    // WM_APP+0x209 is the client's existing null-terminated XML ingress.
    // Copy only complete public lobby records before native code mutates them.
    if(msg==0x8209&&lp){const auto xml=reinterpret_cast<const char*>(lp);size_t size=strnlen_s(xml,1024*1024+1);if(size<=1024*1024)current->observe(std::string_view(xml,size));}
    if(msg==initializeMessage){current->initialize();return 0;}
    if(msg==disconnectedLoginMessage){current->disconnectedLogin();return 0;}
    if(msg==resumeJoinMessage){
        if(wp==current->joinGeneration&&current->queuedJoin>=0){
            const int type=current->queuedJoin;current->queuedJoin=-1;current->join(type);
        }
        return 0;
    }
    if(msg==retireNetworkMessage){
        auto network=reinterpret_cast<HWND>(wp);
        if((current->reconnect==Session::Reconnect::Abandoned||current->retryLoginQueued)&&network==GetDlgItem(GetAncestor(w,GA_ROOT),3006)&&classIs(network,L"WC_CLIENT"))DestroyWindow(network);
        return 0;
    }
    if(msg==repaintNativeMessage){current->repaintNative();return 0;}
    if(msg==nativeCertificateChangedMessage){current->certificateDismissPending=false;current->show();return 0;}
    if(msg==nativeCertificateReadyMessage){current->showCertificate();return 0;}
    if(msg==prepareReturnMessage){current->prepareReturn();return 0;}
    if(msg==finishReturnMessage){current->finishReturn();return 0;}
    if(msg==openNativeViewerMessage){current->openPendingViewer();return 0;}
    if(msg==nativeOpenFinishedMessage){auto pending=std::find(current->pendingNativeOpens.begin(),current->pendingNativeOpens.end(),static_cast<unsigned>(wp));if(pending!=current->pendingNativeOpens.end())current->pendingNativeOpens.erase(pending);return 0;}
    if(msg==WM_TIMER&&wp==bridgeTimer){current->pollReturn();current->pollLogin();current->pollRegistration();current->pollAccountLogs();current->pollWatch();current->show();return 0;}
    if(msg==commandMessage){std::deque<std::wstring> pending;pending.swap(current->commands);for(const auto& command:pending)if(!current->closing)current->command(command);return 0;}
    if(msg==WM_NCDESTROY){current->closing=true;KillTimer(w,bridgeTimer);RemoveWindowSubclass(GetAncestor(w,GA_ROOT),rootObserver,id);if(current->controller)current->controller->Close();current->browser.Reset();current->controller.Reset();sessions.erase(w);RemoveWindowSubclass(w,viewObserver,id);return DefSubclassProc(w,msg,wp,lp);}
    auto result=DefSubclassProc(w,msg,wp,lp);
    if(msg==0x8209)current->queueCertificate();
    if(msg==WM_SIZE||msg==WM_WINDOWPOSCHANGED){current->bounds();current->show();}
    return result;
}
void attach(HWND view) {
    if(sessions.count(view))return;
    auto session=std::make_shared<Session>();session->view=view;
    RECT bounds{};GetClientRect(view,&bounds);
    session->host=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,bounds.right,bounds.bottom,view,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!session->host)return;
    sessions[view]=session;SetWindowSubclass(session->host,coverObserver,viewSubclass,0);SetWindowSubclass(view,viewObserver,viewSubclass,0);
    SetWindowSubclass(GetAncestor(view,GA_ROOT),rootObserver,viewSubclass,reinterpret_cast<DWORD_PTR>(view));SetTimer(view,bridgeTimer,100,nullptr);
    if(auto loading=GetDlgItem(GetAncestor(view,GA_ROOT),4001);classIs(loading,L"WC_RESINIT"))attachLoading(loading);
    if(HMENU menu=GetSystemMenu(GetAncestor(view,GA_ROOT),FALSE)){if(GetMenuState(menu,nativeSettingsCommand,MF_BYCOMMAND)==static_cast<UINT>(-1)){InsertMenuW(menu,SC_CLOSE,MF_BYCOMMAND|MF_STRING,nativeSettingsCommand,L"设置");InsertMenuW(menu,SC_CLOSE,MF_BYCOMMAND|MF_SEPARATOR,0,nullptr);}}
    if(HWND welcome=GetDlgItem(view,3007))attachWelcome(welcome);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);PostMessageW(view,initializeMessage,0,0);
}
LRESULT CALLBACK networkObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    std::shared_ptr<PendingSocketClose> retired;
    auto completed=msg==WM_NCDESTROY?certificateSession(w):nullptr;
    if(msg==0x8301||msg==0x8201){
        const auto root=GetAncestor(w,GA_ROOT);
        std::shared_ptr<Session> current;
        for(auto& entry:sessions)if(!entry.second->closing&&GetAncestor(entry.first,GA_ROOT)==root){current=entry.second;break;}
        if(current){
            if(w!=GetDlgItem(root,3006)||current->reconnect==Session::Reconnect::Abandoned)return 0;
            if(msg==0x8301&&HIWORD(lp)&&current->gameReturn.pending()){
                current->gameReturn.stop();returnTrace("return DNS failure retained result/account UI");return 0;
            }
            if(msg==0x8301&&HIWORD(lp)&&current->reconnect!=Session::Reconnect::None){
                // WSAAsyncGetHostByName reports failure here without emitting
                // FD_CONNECT. The old path displayed get host failed and left
                // the Web login locked until its 20-second deadline expired.
                diagnostic("Web lobby: DNS failed during recovery; releasing login immediately.");
                current->failReconnect();return 0;
            }
        }
        if(msg==0x8201&&LOWORD(lp)==32){
            // The native FD_CLOSE branch invalidates the global before it
            // forwards to root, so its destructor subsequently skips close.
            // Ignore stale events: they must not clear a newer live socket.
            if(!nativeSocket||*nativeSocket==MAXDWORD||*nativeSocket!=static_cast<DWORD>(wp))return 0;
            retired=std::make_shared<PendingSocketClose>(PendingSocketClose{static_cast<DWORD>(wp)});
            pendingSocketCloses[w]=retired;
        }
    }
    const auto result=DefSubclassProc(w,msg,wp,lp);
    if(retired){retired->close();pendingSocketCloses.erase(w);}
    if(msg==WM_NCDESTROY){
        // A nested root callback may destroy this window before FD_CLOSE
        // unwinds. Release once, before a backend can retire the same socket.
        if(auto found=pendingSocketCloses.find(w);found!=pendingSocketCloses.end()){found->second->close();pendingSocketCloses.erase(found);}
        LobbyConnection::ReleaseWindow(w);RemoveWindowSubclass(w,networkObserver,id);
        if(completed&&!completed->closing&&completed->gameReturn.waitingForOK()&&!completed->gameReturn.prepared())
            PostMessageW(completed->view,prepareReturnMessage,0,0);
    }
    return result;
}
HWND WINAPI createA(DWORD ex,LPCSTR cls,LPCSTR title,DWORD style,int x,int y,int width,int height,HWND parent,HMENU menu,HINSTANCE module,LPVOID param) {
    const bool legacy=reinterpret_cast<UINT_PTR>(cls)>0xffff&&(!strcmp(cls,"WC_LOBBY")||!strcmp(cls,"WC_WELCOME"));
    const bool certificate=reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_ITEM_POPUP")&&certificateArgument(param);
    if(certificate)if(auto current=certificateSession(parent);current&&current->gameReturn.completedGame()){
        current->rememberCertificate(static_cast<const char*>(param));
        // Result OK owns the transition to recognition. A later native HELO
        // must not show that same recognition a second time.
        if(current->gameReturn.waitingForOK()||!current->returnNavigationReady||current->certificateDelivered)return nullptr;
        current->show();
    }
    HWND w=previousCreate(ex,cls,title,legacy?style&~WS_VISIBLE:style,x,y,width,height,parent,menu,module,param);
    if(w&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_CLIENT"))SetWindowSubclass(w,networkObserver,viewSubclass,0);
    if(w&&certificate)attachCertificate(w);
    if(w&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_RESINIT"))attachLoading(w);
    if(w&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_VIEW"))attach(w);
    if(w&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_LOBBY"))attachLobby(w);
    if(w&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_WELCOME"))attachWelcome(w);
    if(w&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_GAME"))SetWindowSubclass(w,gameObserver,viewSubclass,0);
    if(w&&legacy&&(style&WS_VISIBLE)){auto current=owner(w);SetWindowPos(w,current?current->host:HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOACTIVATE);}
    return w;
}
BOOL WINAPI postDisconnectedLogin(HWND w,UINT,WPARAM,LPARAM){
    if(auto current=threadSession())return PostMessageW(current->view,disconnectedLoginMessage,0,0);
    return PostMessageA(GetAncestor(w,GA_ROOT),0x8204,0,reinterpret_cast<LPARAM>("GOWELCOMEBYERR"));
}
using NativeMessageBox=int(WINAPI*)(HWND,LPCSTR,LPCSTR,UINT);
NativeMessageBox* disconnectedMessageSlot=nullptr;
HHOOK disconnectedNoticeHook=nullptr;
LRESULT CALLBACK labelDisconnectedNotice(int code,WPARAM wp,LPARAM lp){
    const auto next=CallNextHookEx(nullptr,code,wp,lp);
    wchar_t cls[16]{};
    if(code==HCBT_ACTIVATE&&GetClassNameW(reinterpret_cast<HWND>(wp),cls,16)&&!wcscmp(cls,L"#32770")){
        // The single MB_OK button keeps the system YES caption and mnemonic.
        using MessageString=LPCWSTR(WINAPI*)(UINT);
        const auto text=reinterpret_cast<MessageString>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"MB_GetString"));
        const LPCWSTR yes=text?text(IDYES-1):nullptr;
        HWND button=GetDlgItem(reinterpret_cast<HWND>(wp),IDOK);if(!button)button=GetDlgItem(reinterpret_cast<HWND>(wp),IDCANCEL);
        if(button)SetWindowTextW(button,yes&&*yes?yes:L"&Yes");
        UnhookWindowsHookEx(disconnectedNoticeHook);disconnectedNoticeHook=nullptr;
    }
    return next;
}
int WINAPI showDisconnectedNotice(HWND w,LPCSTR text,LPCSTR title,UINT flags){
    // Staying at the table after CONNECTION CLOSED leaves it permanently
    // offline, so the dialog offers only YES (return to login).
    disconnectedNoticeHook=SetWindowsHookExW(WH_CBT,labelDisconnectedNotice,nullptr,GetCurrentThreadId());
    (*disconnectedMessageSlot)(w,text,title,(flags&~MB_TYPEMASK)|MB_OK);
    if(disconnectedNoticeHook){UnhookWindowsHookEx(disconnectedNoticeHook);disconnectedNoticeHook=nullptr;}
    return IDYES;
}
bool patchDisconnectedLogin(BYTE* base){
    // Native CONNECTION CLOSED dialog: show a single YES button through the
    // original (localized) MessageBoxA import, then route YES to login.
    constexpr unsigned messageSite=0x614e5,site=0x614f7;
    const BYTE messageExpected[]={0xff,0x15,0x98,0x54,0x4e,0x00};
    const BYTE expected[]={0xff,0x15,0x2c,0x53,0x4e,0x00};
    if(memcmp(base+messageSite,messageExpected,sizeof(messageExpected))||memcmp(base+site,expected,sizeof(expected)))return false;
    disconnectedMessageSlot=reinterpret_cast<NativeMessageBox*>(base+0xe5498);
    auto writeCall=[&](unsigned rva,const void* target){
        BYTE patch[6]={0xe8,0,0,0,0,0x90};
        const auto relative=reinterpret_cast<const BYTE*>(target)-(base+rva+5);
        memcpy(patch+1,&relative,4);
        DWORD old=0;if(!VirtualProtect(base+rva,sizeof(patch),PAGE_EXECUTE_READWRITE,&old))return false;
        memcpy(base+rva,patch,sizeof(patch));DWORD ignored=0;
        VirtualProtect(base+rva,sizeof(patch),old,&ignored);FlushInstructionCache(GetCurrentProcess(),base+rva,sizeof(patch));
        return true;
    };
    if(!writeCall(site,reinterpret_cast<const void*>(postDisconnectedLogin)))return false;
    if(!writeCall(messageSite,reinterpret_cast<const void*>(showDisconnectedNotice))){
        DWORD old=0;if(VirtualProtect(base+site,sizeof(expected),PAGE_EXECUTE_READWRITE,&old)){memcpy(base+site,expected,sizeof(expected));DWORD ignored=0;VirtualProtect(base+site,sizeof(expected),old,&ignored);}
        return false;
    }
    return true;
}
bool patchGameReturnCalls(BYTE* base){
    // Two table-exit BYEs, two native HELO builders, GOK, result OK and the
    // native owari connection close. BYE must precede that close, not OK.
    // Explicit logout, initial socket setup, authentication and game packets
    // keep using the original sender. The known-client hash is checked upstream.
    constexpr unsigned sites[]={0x2156e,0x7bbd2,0x644fe,0x6476a,0x6291b};
    for(auto rva:sites){int relative=0;memcpy(&relative,base+rva+1,4);if(base[rva]!=0xe8||base+rva+5+relative!=reinterpret_cast<BYTE*>(nativeSend))return false;}
    constexpr unsigned resultSite=0x51b5b,closeSite=0x92f67;
    const BYTE resultExpected[]={0xff,0x15,0x2c,0x53,0x4e,0x00};
    if(memcmp(base+resultSite,resultExpected,sizeof(resultExpected))||memcmp(base+closeSite,resultExpected,sizeof(resultExpected)))return false;
    auto writeCall=[&](unsigned rva,SendXml target){
        BYTE patch[5]={0xe8};const auto relative=reinterpret_cast<BYTE*>(target)-(base+rva+5);memcpy(patch+1,&relative,4);
        DWORD old=0;if(!VirtualProtect(base+rva,5,PAGE_EXECUTE_READWRITE,&old))return false;
        memcpy(base+rva,patch,5);DWORD ignored=0;VirtualProtect(base+rva,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),base+rva,5);return true;
    };
    for(unsigned i=0;i<5;++i)if(!writeCall(sites[i],i<2?leaveGame:sendGameReturn)){while(i)writeCall(sites[--i],nativeSend);return false;}
    auto writePost=[&](unsigned rva,decltype(&postResultReturn) target){
        DWORD old=0;if(!VirtualProtect(base+rva,6,PAGE_EXECUTE_READWRITE,&old))return false;
        BYTE patch[6]={0xe8,0,0,0,0,0x90};
        const auto relative=reinterpret_cast<BYTE*>(target)-(base+rva+5);memcpy(patch+1,&relative,4);
        memcpy(base+rva,patch,6);DWORD ignored=0;VirtualProtect(base+rva,6,old,&ignored);FlushInstructionCache(GetCurrentProcess(),base+rva,6);return true;
    };
    if(!writePost(resultSite,postResultReturn)){for(auto site:sites)writeCall(site,nativeSend);return false;}
    if(!writePost(closeSite,postGameConnectionClose)){
        DWORD old=0;if(VirtualProtect(base+resultSite,6,PAGE_EXECUTE_READWRITE,&old)){memcpy(base+resultSite,resultExpected,6);DWORD ignored=0;VirtualProtect(base+resultSite,6,old,&ignored);FlushInstructionCache(GetCurrentProcess(),base+resultSite,6);}
        for(auto site:sites)writeCall(site,nativeSend);return false;
    }
    return true;
}
bool patchReturnProfileGate(BYTE* base){
    constexpr unsigned site=0x62ebf,end=0x64ebf;
    const BYTE expected[]={0x8b,0x45,0x24,0x85,0xc0};
    const BYTE endExpected[]={0x8b,0x8c,0x24,0x68,0x08,0x00,0x00};
    if(memcmp(base+site,expected,sizeof(expected))||memcmp(base+end,endExpected,sizeof(endExpected)))return false;
    DWORD old=0;if(!VirtualProtect(base+site,5,PAGE_EXECUTE_READWRITE,&old))return false;
    nativeProfileEnd=base+end;
    BYTE patch[5]={0xe8};const auto relative=reinterpret_cast<BYTE*>(nativeProfileGate)-(base+site+5);memcpy(patch+1,&relative,4);
    memcpy(base+site,patch,5);DWORD ignored=0;VirtualProtect(base+site,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),base+site,5);return true;
}
}
bool ShowNotice(HWND window,Notice notice){
    for(auto& entry:sessions){
        auto& session=*entry.second;
        if(session.closing||session.uiThread!=GetCurrentThreadId()||!session.ready||!session.rendered||!session.loggedIn||!session.browser||!visible(session.host)||!session.controllerVisible||session.backgroundOnly)continue;
        if(window&&GetAncestor(window,GA_ROOT)!=GetAncestor(session.view,GA_ROOT))continue;
        const char* kind=notice==Notice::ConnectionClosed?"connectionClosed":notice==Notice::MembershipExpired?"membershipExpired":"eligibilityRejected";
        const auto json=LobbyProtocol::Wide(std::string("{\"type\":\"nativeNotice\",\"kind\":\"")+kind+"\"}");
        return SUCCEEDED(session.browser->PostWebMessageAsJson(json.c_str()));
    }
    return false;
}
bool Install(HMODULE executable,const std::wstring& root,bool supported) {
    if(!supported||!GetPrivateProfileIntW(L"lobby",L"webview2",1,(root+L"\\enhanced.ini").c_str()))return false;
    directory=root;
    if(GetFileAttributesW((root+L"\\web\\index.html").c_str())==INVALID_FILE_ATTRIBUTES){diagnostic("Web lobby: local UI missing; original lobby retained.");return false;}
    auto base=reinterpret_cast<BYTE*>(executable);
    // In addition to the whole-file hash checked by the caller, validate the
    // one internal entry point used for original-client XML commands.
    const BYTE expected[]={0x53,0x8b,0x1d,0x3c,0x4e,0x4f,0x00};
    if(memcmp(base+0x15ac0,expected,sizeof(expected))){diagnostic("Web lobby: native sender signature mismatch.");return false;}
    nativeSend=reinterpret_cast<SendXml>(base+0x15ac0);
    nativeSocket=reinterpret_cast<const DWORD*>(base+0xf4e3c);
    PeImports::Visit(executable,[](const PeImports::Import& entry){
        if(!_stricmp(entry.library,"wsock32.dll")&&((entry.name&&!strcmp(entry.name,"closesocket"))||(!entry.name&&entry.ordinal==3)))nativeCloseSocketSlot=entry.slot;
        return true;
    });
    if(!nativeCloseSocketSlot){diagnostic("Web lobby: native closesocket import missing.");return false;}
    loader=LoadLibraryExW((root+L"\\WebView2Loader.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!loader){diagnostic("Web lobby: WebView2Loader.dll missing; original lobby retained.");return false;}
    const bool hooked=PeImports::Hook(executable,"CreateWindowExA",createA,previousCreate);
    if(hooked){
        diagnostic("Web lobby: native creation and lobby event bridge installed.");
        diagnostic("Web lobby: FD_CLOSE socket cleanup and bounded active-game recovery enabled.");
        diagnostic(patchDisconnectedLogin(base)?"Web lobby: native disconnect dialog offers YES only; YES returns to login.":"Web lobby: disconnect call signature unavailable; original behavior retained.");
        returnHooks=patchGameReturnCalls(base);
        returnPrefetchHooks=returnHooks&&patchReturnProfileGate(base);
        returnTrace(returnPrefetchHooks?"native return profile prefetch enabled; AUTH retained; UI deferred to result OK":"return profile prefetch unavailable; ordinary result flow retained");
        returnTrace(returnHooks?"result-intent and same-connection handoff hooks installed":"call signatures unavailable; original return flow retained");
    }
    return hooked;
}
}
