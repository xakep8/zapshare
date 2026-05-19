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

#include "json/json.hpp"
#include "net/httplib.h"
#include "types.h"

using json = nlohmann::json;
namespace ip = asio::ip;
using asio::ip::udp;

struct PublicEndpoint {
    std::string ip;
    uint16_t port;
    std::string local_ip;  // New
    uint16_t local_port;   // New
};

namespace Utils {

constexpr char STUN_ENDPOINT[] = "stun.l.google.com";
constexpr char STUN_PORT[] = "19302";
constexpr size_t DEFAULT_PORT = 5173;

inline const char* get_env(const char* key) {
    const char* value = std::getenv(key);
    if (!value) {
        throw std::runtime_error(std::string("Missing environment variable: ") +
                                 key);
    }
    return value;
}

inline const char* CENTRAL_SERVER_URL = get_env("CENTRAL_SERVER_URL");

inline TRANSFERS transfer_metadata_from_json(const json& data) {
    TRANSFERS t;
    t.id = data["id"];

    // Unpack sender_ip (Format: PublicIP;LocalIP:LocalPort)
    std::string raw_ip = data["sender_ip"];
    size_t semi = raw_ip.find(';');
    if (semi != std::string::npos) {
        t.sender_ip = raw_ip.substr(0, semi);
        std::string local_part = raw_ip.substr(semi + 1);
        size_t colon = local_part.find(':');
        if (colon != std::string::npos) {
            t.sender_local_ip = local_part.substr(0, colon);
            t.sender_local_port = std::stoi(local_part.substr(colon + 1));
        } else {
            t.sender_local_ip = local_part;
            t.sender_local_port = DEFAULT_PORT;
        }
    } else {
        t.sender_ip = raw_ip;
        t.sender_local_ip = "";
        t.sender_local_port = 0;
    }

    t.sender_port = data["sender_port"];
    // Fallback or explicit fields if server supports them in future
    if (t.sender_local_ip.empty()) {
        t.sender_local_ip =
            data.contains("sender_local_ip") ? data["sender_local_ip"] : "";
        t.sender_local_port = data.contains("sender_local_port")
                                  ? data["sender_local_port"].get<uint32_t>()
                                  : 0;
    }

    t.protocol = data["protocol"];
    t.file_name = data["file_name"];
    t.file_size = data["file_size"];
    t.file_hash = data["file_hash"];
    t.token = data["token"];
    return t;
}

inline bool check_file_exists(const std::string_view& filepath) {
    std::filesystem::path file{filepath};
    return std::filesystem::exists(file);
}

inline bool look_up(const std::string_view secret) {
    httplib::Client client(CENTRAL_SERVER_URL);
    const std::string url = "/lookup/" + std::string(secret);
    if (auto res = client.Get(url)) {
        if (res->body == "True") {
            return true;
        }
    }
    return false;
}

inline TRANSFERS get_transfer_metadata(const std::string_view& secret) {
    httplib::Client client(CENTRAL_SERVER_URL);
    const std::string url = "/getfile/" + std::string(secret);
    if (auto res = client.Get(url)) {
        return transfer_metadata_from_json(json::parse(res->body));
    }
    throw std::runtime_error(
        "[GET TRANSFER METADATA] Error getting metadata for the transfer");
}

inline std::string get_local_ip_address() {
    asio::io_context io_ctx;
    ip::udp::resolver resolver(io_ctx);
    // Connect to a well-known public IP (e.g., Google DNS 8.8.8.8) to let
    // the OS determine the correct *local* endpoint for an outgoing
    // connection.
    auto results = resolver.resolve(ip::udp::v4(), "8.8.8.8", "53");
    ip::udp::endpoint ep = *results.begin();
    ip::udp::socket socket(io_ctx);
    socket.connect(ep);
    ip::address addr = socket.local_endpoint().address();
    return addr.to_string();
}

// Discover public IP:port via STUN (XOR-MAPPED-ADDRESS)
inline PublicEndpoint get_public_endpoint_for_socket(asio::io_context& io,
                                                     udp::socket& socket) {
    ip::udp::endpoint stun_endpoint =
        *udp::resolver(io).resolve(udp::v4(), STUN_ENDPOINT, STUN_PORT).begin();
    std::array<uint8_t, 20> req = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
    for (int i = 8; i < 20; ++i) req[i] = rand();
    socket.send_to(asio::buffer(req), stun_endpoint);

    std::array<uint8_t, 1024> buf;
    udp::endpoint sender;
    size_t n = socket.receive_from(asio::buffer(buf), sender);

    PublicEndpoint my_ep;
    // Local IP/Port (from socket)
    my_ep.local_ip = socket.local_endpoint().address().to_string();
    my_ep.local_ip = get_local_ip_address();
    my_ep.local_port = socket.local_endpoint().port();

    // STUN Parsing (Short)
    bool stun_success = false;
    for (size_t i = 20; i + 8 <= n;) {
        uint16_t t = (buf[i] << 8) | buf[i + 1],
                 l = (buf[i + 2] << 8) | buf[i + 3];
        if (t == 0x20 && l >= 8) {
            uint32_t xk = 0x2112A442;
            uint16_t p = ((buf[i + 6] << 8) | buf[i + 7]) ^ (xk >> 16);
            uint32_t a = ((buf[i + 8] << 24) | (buf[i + 9] << 16) |
                          (buf[i + 10] << 8) | buf[i + 11]) ^
                         xk;
            my_ep.port = p;
            my_ep.ip = std::to_string(a >> 24) + "." +
                       std::to_string((a >> 16) & 0xFF) + "." +
                       std::to_string((a >> 8) & 0xFF) + "." +
                       std::to_string(a & 0xFF);
            stun_success = true;
            break;
        }
        i += 4 + l + ((4 - (l % 4)) % 4);
    }

    if (!stun_success) throw std::runtime_error("STUN Parse failed");

    return my_ep;
}

inline PublicEndpoint get_public_endpoint() {
    asio::io_context io;
    udp::socket socket(io);
    socket.open(udp::v4());
    socket.bind(udp::endpoint(udp::v4(), 0));
    return get_public_endpoint_for_socket(io, socket);
}

// Register discovered public endpoint with central server (Signaling)
inline void signal_receiver_endpoint(const std::string& id,
                                     const PublicEndpoint& endpoint) {
    httplib::Client client(CENTRAL_SERVER_URL);
    json payload = {{"public_ip", endpoint.ip},
                    {"public_port", endpoint.port},
                    {"local_ip", endpoint.local_ip},
                    {"local_port", endpoint.local_port}};
    auto res = client.Post(("/signal/" + id).c_str(), payload.dump(),
                           "application/json");
    if (!res || res->status != 200) {
        std::cerr << "Failed to signal receiver endpoint to server\n";
    }
}

// Poll for receiver's public endpoint (Signaling)
inline PublicEndpoint poll_for_signal(const std::string& id) {
    httplib::Client client(CENTRAL_SERVER_URL);
    std::string url = "/signal/" + id;

    for (int i = 0; i < 30; ++i) {  // Try for 30 seconds
        if (auto res = client.Get(url.c_str())) {
            if (res->status == 200) {
                try {
                    json data = json::parse(res->body);
                    PublicEndpoint ep;
                    ep.ip = data["public_ip"];
                    ep.port = data["public_port"];
                    ep.local_ip =
                        data.contains("local_ip") ? data["local_ip"] : "";
                    ep.local_port = data.contains("local_port")
                                        ? data["local_port"].get<uint16_t>()
                                        : 0;
                    return ep;
                } catch (std::exception& e) {
                    std::cerr << "Poll parse error: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Poll parse error: Unknown" << std::endl;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    throw std::runtime_error("No Signal Received! Timed out");
}

// Perform UDP hole punching to peer's public endpoint using an EXISTING socket
// Now tries both Public and Local
inline void perform_udp_hole_punch(ip::udp::socket& socket,
                                   const PublicEndpoint& peer_endpoint) {
    try {
        std::vector<ip::udp::endpoint> candidates;
        candidates.emplace_back(asio::ip::make_address(peer_endpoint.ip),
                                peer_endpoint.port);

        if (!peer_endpoint.local_ip.empty() && peer_endpoint.local_port != 0) {
            candidates.emplace_back(
                asio::ip::make_address(peer_endpoint.local_ip),
                peer_endpoint.local_port);
        }

        std::string punch_msg = "PUNCH";
        // Send multiple punches to all candidates
        for (int i = 0; i < 5; ++i) {
            for (const auto& endpoint : candidates) {
                try {
                    socket.send_to(asio::buffer(punch_msg), endpoint);
                } catch (...) {
                }  // Ignore send errors (e.g. unreachable)
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (std::exception& e) {
        std::cerr << "Hole punch error: " << e.what() << "\n";
    }
}

// Generate a UUID v4 token (RFC 4122) using std::random_device
inline std::string generate_uuid_token() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis32(0, 0xFFFFFFFF);
    auto rnd32 = [&]() { return dis32(gen); };

    uint32_t d0 = rnd32();
    uint16_t d1 = static_cast<uint16_t>(rnd32() & 0xFFFF);
    uint16_t d2 =
        static_cast<uint16_t>((rnd32() & 0x0FFF) | 0x4000);  // version 4
    uint16_t d3 =
        static_cast<uint16_t>((rnd32() & 0x3FFF) | 0x8000);  // variant 10xxxxxx
    uint64_t d4_hi = rnd32();
    uint64_t d4_lo = rnd32();

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
        << d0 << "-" << std::setw(4) << d1 << "-" << std::setw(4) << d2 << "-"
        << std::setw(4) << d3 << "-" << std::setw(8)
        << static_cast<uint32_t>(d4_hi) << std::setw(8)
        << static_cast<uint32_t>(d4_lo);
    return oss.str();
}

// Register transfer metadata with rendezvous server
inline void register_transfer(const TRANSFERS& t) {
    httplib::Client client(CENTRAL_SERVER_URL);

    // Pack Local IP into sender_ip for legacy server compatibility
    std::string packed_ip = t.sender_ip;
    if (!t.sender_local_ip.empty()) {
        packed_ip +=
            ";" + t.sender_local_ip + ":" + std::to_string(t.sender_local_port);
    }

    json payload = {{"id", t.id},
                    {"sender_ip", packed_ip},
                    {"sender_port", t.sender_port},
                    {"sender_local_ip", t.sender_local_ip},
                    {"sender_local_port", t.sender_local_port},
                    {"protocol", t.protocol},
                    {"file_name", t.file_name},
                    {"file_size", t.file_size},
                    {"file_hash", t.file_hash},
                    {"token", t.token}};
    client.Post("/register", payload.dump(), "application/json");
}
}  // namespace Utils
