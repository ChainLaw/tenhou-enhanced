#pragma once
#include <windows.h>
#include <string>
namespace InputPolicy {
constexpr int escapePassControl=0x7b11,escapeDiscardControl=0x7b12;
bool Install(HMODULE executable,const std::wstring& directory);
void AttachThread();
void DetachThread();
bool HandleEscape(HWND game,LPARAM keyFlags);
}
