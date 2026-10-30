#pragma once
#include <windows.h>
#include <string>
namespace WebLobby {
enum class Notice { ConnectionClosed, MembershipExpired, EligibilityRejected };
bool ShowNotice(HWND owner, Notice notice);
bool Install(HMODULE executable,const std::wstring& root,bool supportedClient);
}
