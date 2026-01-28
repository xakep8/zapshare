#include <iostream>

#include "asio.hpp"
#include "client.hpp"
#include "crypto.hpp"
#include "error.hpp"
#include "server.hpp"
#include "types.h"
#include "utils.hpp"

void start_server(const std::string& file_path, const std::string& transfer_id) {
    asio::io_context io;
    Server s(io, 5173, file_path);
    // Server run will poll for signal and then start
    // However, s.run() is blocking or async?
    // My implementation of s.run() calls do_receive() AND polls blocking.
    // It's mix. Poll is blocking. Then do_receive is async.
    // So we call s.run(), then io.run().
    s.run(transfer_id);
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
        transfer.protocol = "udp";
        transfer.sender_port = 5173; 
        transfer.token = Utils::generate_uuid_token();
        transfer.id = transfer.token; 

        // Discover public endpoint via STUN
        try {
            PublicEndpoint public_ep = Utils::get_public_endpoint();
            transfer.sender_ip = public_ep.ip;
            std::cout << "Public endpoint: " << public_ep.ip << ":" << public_ep.port << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "NAT traversal failed: " << e.what() << std::endl;
            // Proceed anyway, might work locally
             transfer.sender_ip = "127.0.0.1";
        }

        // Register this transfer with rendezvous server
        Utils::register_transfer(transfer);
        std::cout << "Your secret is: " << transfer.id << " share this with the receiver!!\n";

        // Start server
        start_server(std::string(filepath), transfer.id);
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

        std::string host_override = std::getenv("ZAPSHARE_HOST_OVERRIDE") ? std::getenv("ZAPSHARE_HOST_OVERRIDE") : "";
        std::string connect_host = host_override.empty() ? peer_transfer.sender_ip : host_override;
        
        // Use Sender's IP and Port (5173 or whatever registered)
        if (!run_client_session(connect_host, static_cast<uint16_t>(peer_transfer.sender_port), std::string(secret),
                                peer_transfer.file_name)) {
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
