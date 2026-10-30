#pragma once
#include <string>

// Blocking IO helpers. The WebView session owns scheduling and stale-result
// checks; these functions do not access windows or mutable session state.
namespace LobbyServices {
struct WatchListResult { int room=0; std::string response; bool error=false; };
struct RegistrationResult { int sequence=0; std::string response,error; };
struct AccountLogsResult { int sequence=0; std::wstring account; std::string response; bool error=false; };

WatchListResult FetchWatchList(int room);
RegistrationResult RegisterName(int sequence,bool query,const std::string& name);
AccountLogsResult FindAccountLogs(int sequence,const std::wstring& account);
}
