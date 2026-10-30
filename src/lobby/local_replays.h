#pragma once
#include <string>

namespace LobbyServices {
struct ReplayMessages { std::string history,files; };
ReplayMessages ReadLocalReplays(const std::wstring& directory);
}
