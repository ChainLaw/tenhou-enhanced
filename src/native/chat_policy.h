#pragma once
#include <windows.h>
#include <string>
namespace ChatPolicy {
bool Install(HMODULE executable,const std::wstring& directory);
void AttachRoot(HWND root);
void EnhanceSettingsPage(HWND page);
constexpr int roomPopupControl=0x7b10;
}
