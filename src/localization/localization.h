#pragma once
#include <windows.h>
#include <string>
#include <string_view>
namespace Localization {
std::wstring Translate(std::wstring_view text, bool* recognized=nullptr);
std::wstring DecodeDisplay(const char* text,int count=-1);
unsigned Install(HMODULE module);
}
