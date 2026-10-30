#pragma once
#include <string>
#include <string_view>
namespace LobbyProtocol {
std::string DecodeCounts(std::string_view value);
std::string Quote(std::string_view value);
std::string UrlEncode(std::string_view value);
std::string Tag(std::string_view xml);
std::string FilterEvent(std::string_view xml);
bool Integer(std::string_view value,int minimum,int maximum,int& result);
int ReservationIndex(int type);
unsigned NormalizeWatchFlags(unsigned flags);
std::string Utf8(const std::wstring& value);
std::wstring Wide(std::string_view value);
}
