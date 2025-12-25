#pragma once
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "httplib.h"
#include "json.hpp"
#include "types.h"

using json = nlohmann::json;

namespace Error {
void print_usage() { std::cerr << "usage:\n" << "    zapshare send [filepath]\n" << "    zapshare get [secret]\n"; }

void invalid_secret() {
    std::cerr << "Invalid secret!\n";
    print_usage();
}

void invalid_file_path() {
    std::cerr << "Invalid filepath!\n";
    print_usage();
}
}  // namespace Error

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

bool check_file_exists(const std::string_view& filepath) {
    std::filesystem::path file{filepath};
    return std::filesystem::exists(file);
}

bool look_up(const std::string_view secret) {
    httplib::Client client("http://127.0.0.1:3000");
    const std::string url = "/lookup/" + std::string(secret);
    if (auto res = client.Get(url)) {
        if (res->body == "True") {
            return true;
        }
    }
    return false;
}

TRANSFERS get_transfer_metadata(const std::string_view& secret) {
    httplib::Client client("http://127.0.0.1:3000");
    const std::string url = "/getfile/" + std::string(secret);
    if (auto res = client.Get(url)) {
        return transfer_metadata_from_json(json::parse(res->body));
    }
    throw std::runtime_error("[GET TRANSFER METADATA] Error getting metadata for the transfer");
}

std::string get_connection_details() {
    // TODO: Implement retrieval of connection details (e.g., host, port, protocol) instead of returning a stub value.
    return "";
}
}  // namespace Utils

namespace Crypto {
std::string generate_token() {}

std::string compute_file_hash(const std::string_view& path) {
    std::ifstream file(std::string(path), std::ifstream::binary);
    if (!file.is_open()) {
        throw std::runtime_error("[CRYPTO] Error opening file: ");
    }

    SHA256_CTX sha256_context;
    SHA256_Init(&sha256_context);
    const int buffer_size = 4096;
    unsigned char buffer[buffer_size];
    while (file.read(reinterpret_cast<char*>(buffer), buffer_size)) {
        SHA256_Update(&sha256_context, buffer, file.gcount());
    }

    if (file.gcount() > 0) {
        SHA256_Update(&sha256_context, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256_context);
    file.close();

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}
}  // namespace Crypto