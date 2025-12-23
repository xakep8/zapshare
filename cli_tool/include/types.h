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

namespace Message {
// Peer-to-peer protocol (your TCP Server/Client)
inline constexpr std::string_view Hello = "HELLO";  // peer_id=abc123
inline constexpr std::string_view Get = "GET";      // file_id=abc123 offset=0 length=65536
inline constexpr std::string_view Data = "DATA";    // file_id=abc123 offset=0 length=65536 <bytes>
inline constexpr std::string_view Done = "DONE";
inline constexpr std::string_view Error = "ERROR";
}  // namespace Message

typedef struct Transfer_Metadata {
    std::string id;
    std::string sender_ip;
    uint32_t sender_port;
    std::string protocol;
    std::string file_name;
    size_t file_size;
    std::string file_hash;
    std::string token;
} TRANSFERS;