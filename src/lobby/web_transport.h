#pragma once
#include <windows.h>
#include <string>
namespace WebTransport {
bool Enabled();
// Enabled is the preference; Active is the route for the next connection.
// Ordinary lobby/viewer sessions stay on Winsock. Changing the route requires
// rebuilding WC_CLIENT; existing sockets retain their original handlers.
bool Active();
void SetActive(bool active);
bool Connected();
void ReleaseWindow(HWND window);
// Only call after the original executable's SHA-256 has been checked.
bool Install(HMODULE executable,const std::wstring& root);
}
