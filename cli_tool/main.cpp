#include <iostream>

#include "asio.hpp"
#include "client.hpp"
#include "crypto.hpp"
#include "error.hpp"
#include "server.hpp"
#include "types.h"
#include "utils.hpp"

void start_server(const std::string& file_path) {
    asio::io_context io;
    Server s(io, 5173, file_path);
    io.run();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        Error::print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == Command::SEND) {
        if (argc < 3) {
            Error::invalid_file_path();
            return 1;
        }
        const std::string_view filepath = argv[2];
        if (!Utils::check_file_exists(filepath)) {
            Error::invalid_file_path();
            return 1;
        }
        TRANSFERS transfer{};
        transfer.file_name = std::filesystem::path(filepath).filename().string();
        transfer.file_hash = Crypto::compute_file_hash(filepath);
        transfer.file_size = std::filesystem::file_size(filepath);
        transfer.protocol = "tcp";
        transfer.sender_port = 5173;  // fixed TCP listen port
        transfer.token = Utils::generate_uuid_token();
        transfer.id = transfer.token;  // use the generated token as the transfer identifier for GET

        // Discover public endpoint via STUN
        try {
            PublicEndpoint public_ep = Utils::get_public_endpoint();
            transfer.sender_ip = public_ep.ip;
            std::cout << "Public endpoint: " << public_ep.ip << ":" << public_ep.port << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "NAT traversal failed: " << e.what() << std::endl;
            return 1;
        }

        // Register this transfer with rendezvous server (id optional)
        Utils::register_transfer(transfer);
        std::cout << "Your secret is: " << transfer.id << " share this with the receiver!!\n";

        // Start server to accept incoming peer connection
        start_server(std::string(filepath));
    } else if (cmd == Command::GET) {
        if (argc < 3) {
            Error::invalid_secret();
            return 1;
        }
        const std::string_view secret = argv[2];
        if (!Utils::look_up(secret)) {
            Error::invalid_file_path();
            return 1;
        }
        TRANSFERS peer_transfer = Utils::get_transfer_metadata(secret);

        // Discover our own public endpoint
        try {
            PublicEndpoint our_public_ep = Utils::get_public_endpoint();
            std::cout << "Our public endpoint: " << our_public_ep.ip << ":" << our_public_ep.port << std::endl;
            // TODO: Register our endpoint with central server so peer can reach us
            // Utils::register_public_endpoint(secret, our_public_ep);
        } catch (const std::exception& e) {
            std::cerr << "Failed to discover public endpoint: " << e.what() << std::endl;
            return 1;
        }

        // Perform UDP hole punching to peer's public endpoint
        try {
            PublicEndpoint peer_ep{peer_transfer.sender_ip, static_cast<uint16_t>(peer_transfer.sender_port)};
            asio::io_context io_ctx;
            auto hole_punch_socket = Utils::perform_udp_hole_punch(io_ctx, peer_ep);
            std::cout << "NAT traversal successful, hole punch socket ready" << std::endl;
            // TCP download still uses direct connect below; hole punch is kept for future NAT handling.
        } catch (const std::exception& e) {
            std::cerr << "NAT traversal hole punching failed: " << e.what() << std::endl;
            return 1;
        }

        // Connect to peer and download the file
        std::string host_override = std::getenv("ZAPSHARE_HOST_OVERRIDE") ? std::getenv("ZAPSHARE_HOST_OVERRIDE") : "";
        std::string connect_host = host_override.empty() ? peer_transfer.sender_ip : host_override;
        if (!run_client_session(connect_host, static_cast<uint16_t>(peer_transfer.sender_port), std::string(secret),
                                peer_transfer.file_name)) {
            // Fallback for local testing: try localhost if public endpoint fails
            if (host_override.empty()) {
                std::cerr << "Primary connect failed, trying localhost fallback..." << std::endl;
                if (run_client_session("127.0.0.1", static_cast<uint16_t>(peer_transfer.sender_port),
                                       std::string(secret), peer_transfer.file_name)) {
                    return 0;
                }
            }
            std::cerr << "File download failed" << std::endl;
            return 1;
        }
    } else {
        std::cerr << "Invalid Command!\n";
        Error::print_usage();
        return 1;
    }
    return 0;
}
