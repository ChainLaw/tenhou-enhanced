#include "lobby_services.h"
#include "lobby_protocol.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <memory>
#include <utility>

namespace LobbyServices {
namespace {
struct InternetCloser {
    void operator()(void* handle) const { WinHttpCloseHandle(handle); }
};
using InternetHandle=std::unique_ptr<void,InternetCloser>;

struct Response { std::string body; bool ok=false; };
Response get(const wchar_t* host,const std::wstring& path,bool secure,size_t limit,
             const wchar_t* agent=L"Tenhou Windows Lobby",const wchar_t* referer=WINHTTP_NO_REFERER) {
    InternetHandle session(WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,nullptr,nullptr,0));
    if(!session)return {};
    WinHttpSetTimeouts(session.get(),5000,5000,5000,5000);
    InternetHandle connection(WinHttpConnect(session.get(),host,secure?INTERNET_DEFAULT_HTTPS_PORT:INTERNET_DEFAULT_HTTP_PORT,0));
    if(!connection)return {};
    InternetHandle request(WinHttpOpenRequest(connection.get(),L"GET",path.c_str(),nullptr,referer,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,secure?WINHTTP_FLAG_SECURE:0));
    if(!request||!WinHttpSendRequest(request.get(),WINHTTP_NO_ADDITIONAL_HEADERS,0,nullptr,0,0,0)||
       !WinHttpReceiveResponse(request.get(),nullptr))return {};
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                           nullptr,&status,&size,nullptr)||status!=200)return {};

    Response result;
    const auto started=std::chrono::steady_clock::now();
    for(;;) {
        DWORD available=0;
        if(std::chrono::steady_clock::now()-started>std::chrono::seconds(15)||
           !WinHttpQueryDataAvailable(request.get(),&available))return {};
        if(!available){result.ok=true;return result;}
        if(available>limit-result.body.size())return {};
        const auto offset=result.body.size();result.body.resize(offset+available);
        DWORD read=0;
        if(!WinHttpReadData(request.get(),result.body.data()+offset,available,&read))return {};
        result.body.resize(offset+read);
        if(!read){result.ok=true;return result;}
    }
}
}

WatchListResult FetchWatchList(int room) {
    wchar_t code[16]{};swprintf_s(code,room<10000?L"%04d":L"%d",room);
    auto response=get(L"tenhou.net",std::wstring(L"/0/wg/")+code+L".js",true,1024*1024,
                      L"Mozilla/5.0 Tenhou Windows Lobby",L"https://tenhou.net/4/");
    const bool error=!response.ok||response.body.empty();
    return {room,std::move(response.body),error};
}

RegistrationResult RegisterName(int sequence,bool query,const std::string& name) {
    const auto path=LobbyProtocol::Wide(std::string("/regid?")+(query?"q=1&":"")+"uname="+LobbyProtocol::UrlEncode(name));
    auto response=get(L"b.tenhou.net",path,false,8192);
    const bool error=!response.ok||response.body.empty();
    return {sequence,std::move(response.body),error?"无法连接 Windows 客户端的注册服务，请稍后重试。":""};
}

AccountLogsResult FindAccountLogs(int sequence,const std::wstring& account) {
    // Only the session's validated native login ID reaches this endpoint.
    auto response=get(L"tenhou.net",L"/0/log/find.cgi?un="+account+L"&raw=1",true,1024*1024);
    return {sequence,account,std::move(response.body),!response.ok};
}
}
