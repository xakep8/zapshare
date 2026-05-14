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

namespace UdpConfig {
inline constexpr size_t MAX_PACKET_SIZE = 1400;  // Safe MTU
inline constexpr size_t HEADER_SIZE =
    128;  // Increased for safety with string based headers
inline constexpr size_t PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;
inline constexpr int RETRY_TIMEOUT_MS = 200;
inline constexpr int MAX_RETRIES = 20;
}  // namespace UdpConfig

typedef struct Transfer_Metadata {
    std::string id;
    std::string sender_ip;
    uint32_t sender_port;
    std::string sender_local_ip;  // New
    uint32_t sender_local_port;   // New
    std::string protocol;
    std::string file_name;
    size_t file_size;
    std::string file_hash;
    std::string token;
} TRANSFERS;