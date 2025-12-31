#pragma once
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <asio.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include "httplib.h"
#include "json.hpp"
#include "types.h"

using json = nlohmann::json;
namespace ip = asio::ip;

struct PublicEndpoint {
    std::string ip;
    uint16_t port;
};

namespace Utils {
inline TRANSFERS transfer_metadata_from_json(const json& data) {
    try {
        TRANSFERS t;
        t.id = data["id"];
        t.sender_ip = data["sender_ip"];
        t.sender_port = data["sender_port"];
        t.protocol = data["protocol"];
        t.file_name = data["file_name"];
        t.file_size = data["file_size"];
        t.file_hash = data["file_hash"];
        t.token = data["token"];
        return t;
    } catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        throw std::runtime_error(e.what());
    }
}

inline bool check_file_exists(const std::string_view& filepath) {
    std::filesystem::path file{filepath};
    return std::filesystem::exists(file);
}

inline bool look_up(const std::string_view secret) {
    httplib::Client client("http://127.0.0.1:3000");
    const std::string url = "/lookup/" + std::string(secret);
    if (auto res = client.Get(url)) {
        if (res->body == "True") {
            return true;
        }
    }
    return false;
}

inline TRANSFERS get_transfer_metadata(const std::string_view& secret) {
    httplib::Client client("http://127.0.0.1:3000");
    const std::string url = "/getfile/" + std::string(secret);
    if (auto res = client.Get(url)) {
        return transfer_metadata_from_json(json::parse(res->body));
    }
    throw std::runtime_error("[GET TRANSFER METADATA] Error getting metadata for the transfer");
}

inline std::string get_local_ip_address() {
    try {
        asio::io_context io_ctx;
        ip::udp::resolver resolver(io_ctx);
        // Connect to a well-known public IP (e.g., Google DNS 8.8.8.8) to let
        // the OS determine the correct *local* endpoint for an outgoing connection.
        auto results = resolver.resolve(ip::udp::v4(), "8.8.8.8", "53");
        ip::udp::endpoint ep = *results.begin();
        ip::udp::socket socket(io_ctx);
        socket.connect(ep);
        ip::address addr = socket.local_endpoint().address();
        return addr.to_string();
    } catch (std::exception& e) {
        std::cerr << "Could not get IP address. Exception: " << e.what() << std::endl;
        return "Unable to get IP Address";
    }
}
// Discover public IP:port via STUN (XOR-MAPPED-ADDRESS)
inline PublicEndpoint get_public_endpoint(const std::string& stun_server = "stun.l.google.com",
                                          uint16_t stun_port = 19302) {
    try {
        asio::io_context io_ctx;
        ip::udp::resolver resolver(io_ctx);
        auto results = resolver.resolve(ip::udp::v4(), stun_server, std::to_string(stun_port));
        ip::udp::endpoint stun_endpoint = *results.begin();

        ip::udp::socket socket(io_ctx);
        socket.open(ip::udp::v4());

        // STUN Binding Request
        std::array<uint8_t, 20> request{};
        request[0] = 0x00;
        request[1] = 0x01;  // Binding Request
        request[2] = 0x00;
        request[3] = 0x00;  // Length = 0
        request[4] = 0x21;
        request[5] = 0x12;
        request[6] = 0xA4;
        request[7] = 0x42;  // Magic Cookie
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (int i = 8; i < 20; ++i) request[i] = static_cast<uint8_t>(dis(gen));

        socket.send_to(asio::buffer(request), stun_endpoint);

        std::array<uint8_t, 1024> response{};
        ip::udp::endpoint sender_endpoint;
        size_t len = socket.receive_from(asio::buffer(response), sender_endpoint);

        // Parse XOR-MAPPED-ADDRESS (0x0020)
        const uint32_t MAGIC_COOKIE = 0x2112A442;
        for (size_t i = 20; i + 8 <= len;) {
            uint16_t attr_type = (response[i] << 8) | response[i + 1];
            uint16_t attr_len = (response[i + 2] << 8) | response[i + 3];
            size_t attr_start = i + 4;
            if (attr_type == 0x0020 && attr_len >= 8) {
                uint8_t family = response[attr_start + 1];
                if (family == 0x01) {  // IPv4
                    uint16_t xport = (response[attr_start + 2] << 8) | response[attr_start + 3];
                    uint32_t xaddr = (response[attr_start + 4] << 24) | (response[attr_start + 5] << 16) |
                                     (response[attr_start + 6] << 8) | response[attr_start + 7];
                    uint16_t port = static_cast<uint16_t>(xport ^ (MAGIC_COOKIE >> 16));
                    uint32_t addr = xaddr ^ MAGIC_COOKIE;
                    std::string ip_str = std::to_string((addr >> 24) & 0xFF) + "." +
                                         std::to_string((addr >> 16) & 0xFF) + "." +
                                         std::to_string((addr >> 8) & 0xFF) + "." + std::to_string(addr & 0xFF);
                    return PublicEndpoint{ip_str, port};
                }
            }
            // 32-bit padding alignment
            i = attr_start + attr_len;
            if ((i % 4) != 0) i += (4 - (i % 4));
        }
        throw std::runtime_error("Failed to parse STUN response (no XOR-MAPPED-ADDRESS)");
    } catch (const std::exception& e) {
        std::cerr << "STUN error: " << e.what() << std::endl;
        throw;
    }
}

// Register discovered public endpoint with central server
inline void register_public_endpoint(const std::string& token, const PublicEndpoint& endpoint) {
    httplib::Client client("http://127.0.0.1:3000");
    json payload = {{"token", token}, {"public_ip", endpoint.ip}, {"public_port", endpoint.port}};
    client.Post("/register_endpoint", payload.dump(), "application/json");
}

// Perform UDP hole punching to peer's public endpoint
inline ip::udp::socket perform_udp_hole_punch(asio::io_context& io_ctx, const PublicEndpoint& peer_endpoint) {
    ip::udp::socket socket(io_ctx);
    socket.open(ip::udp::v4());
    ip::udp::endpoint peer(asio::ip::make_address(peer_endpoint.ip), peer_endpoint.port);
    std::string punch_msg = "PUNCH";
    for (int i = 0; i < 5; ++i) {
        socket.send_to(asio::buffer(punch_msg), peer);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return socket;  // moved out (non-copyable)
}

// Generate a UUID v4 token (RFC 4122) using std::random_device
inline std::string generate_uuid_token() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis32(0, 0xFFFFFFFF);
    auto rnd32 = [&]() { return dis32(gen); };

    uint32_t d0 = rnd32();
    uint16_t d1 = static_cast<uint16_t>(rnd32() & 0xFFFF);
    uint16_t d2 = static_cast<uint16_t>((rnd32() & 0x0FFF) | 0x4000); // version 4
    uint16_t d3 = static_cast<uint16_t>((rnd32() & 0x3FFF) | 0x8000); // variant 10xxxxxx
    uint64_t d4_hi = rnd32();
    uint64_t d4_lo = rnd32();

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0')
        << std::setw(8) << d0 << "-"
        << std::setw(4) << d1 << "-"
        << std::setw(4) << d2 << "-"
        << std::setw(4) << d3 << "-"
        << std::setw(8) << static_cast<uint32_t>(d4_hi) << std::setw(8) << static_cast<uint32_t>(d4_lo);
    return oss.str();
}

// Register transfer metadata with rendezvous server
inline void register_transfer(const TRANSFERS& t) {
    httplib::Client client("http://127.0.0.1:3000");
    json payload = {
        {"id", t.id},
        {"sender_ip", t.sender_ip},
        {"sender_port", t.sender_port},
        {"protocol", t.protocol},
        {"file_name", t.file_name},
        {"file_size", t.file_size},
        {"file_hash", t.file_hash},
        {"token", t.token}
    };
    client.Post("/register", payload.dump(), "application/json");
}
}  // namespace Utils
