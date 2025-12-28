#include <iostream>

#include "asio.hpp"
#include "server.hpp"
#include "types.h"
#include "utils.hpp"

void start_server() {
    asio::io_context io;
    Server s(io, 5173);
    io.run();
}

void start_client() {
    using asio::ip::tcp;
    asio::io_context io_context;

    // we need a socket and a resolver
    tcp::socket socket(io_context);
    tcp::resolver resolver(io_context);

    // now we can use connect(..)
    asio::connect(socket, resolver.resolve("127.0.0.1", "5173"));
    while (true) {
        std::string data{"some client data ..."};
        auto result = asio::write(socket, asio::buffer(data));

        // the result represents the size of the sent data
        std::cout << "data sent: " << data.length() << '/' << result << std::endl;
    }

    // and close the connection now
    asio::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close();
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
        transfer.sender_ip = Utils::get_local_ip_address();
        transfer.token = "";
        
        // Discover public endpoint via STUN
        try {
            PublicEndpoint public_ep = Utils::get_public_endpoint();
            transfer.sender_ip = public_ep.ip;
            transfer.sender_port = public_ep.port;
            std::cout << "Public endpoint: " << public_ep.ip << ":" << public_ep.port << std::endl;
            // Register with central server (token will be returned/used)
            // Utils::register_public_endpoint(transfer.token, public_ep);
        } catch (const std::exception& e) {
            std::cerr << "NAT traversal failed: " << e.what() << std::endl;
            return 1;
        }
        
        // Start server to accept incoming peer connection
        start_server();
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
            // Now switch to TCP or use the hole-punched UDP socket for data transfer
        } catch (const std::exception& e) {
            std::cerr << "NAT traversal hole punching failed: " << e.what() << std::endl;
            return 1;
        }
        
        start_client();
    } else {
        std::cerr << "Invalid Command!\n";
        Error::print_usage();
        return 1;
    }
    return 0;
}
