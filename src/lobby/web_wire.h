#pragma once
#include <string>
#include <string_view>

namespace WebWire {
// Throws on malformed or unsupported data. Never guess a game event.
std::string ToNative(std::string_view json);
// Native riichi's candidate is a validation hint; discard waits for native D.
std::string ToWeb(std::string_view xml);
}
