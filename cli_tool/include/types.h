#pragma once

#include <string_view>

/*
I need two commands only
./p2p send <file path>
./p2p get <secret>

and I need to validate these two only
so let me see how I can do this
*/

namespace Command {
inline constexpr std::string_view SEND = "send";
inline constexpr std::string_view GET = "get";
}  // namespace Command