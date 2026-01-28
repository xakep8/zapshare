#include "server.hpp"

#include <iostream>

#include "session.hpp"
#include "utils.hpp"

Server::Server(asio::io_context& io_context, short port, const std::string& file_path)
    : m_Initialized(false), m_socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), port)), m_file_path(file_path) {
}

Server::~Server() { std::cout << "Destructor\n"; }

void Server::run(const std::string& transfer_id) {
    m_Initialized = true;
    
    // 1. Poll for signal
    std::cout << "Polling for peer signal..." << std::endl;
    PublicEndpoint peer_ep = Utils::poll_for_signal(transfer_id);
    m_remote_endpoint = asio::ip::udp::endpoint(asio::ip::make_address(peer_ep.ip), peer_ep.port);
    std::cout << "Peer signal received: " << peer_ep.ip << ":" << peer_ep.port << std::endl;

    // 2. Punch hole
    std::cout << "Punching hole to peer..." << std::endl;
    Utils::perform_udp_hole_punch(m_socket, peer_ep);
    
    // 3. Create Session and Start Receive Loop
    m_session = std::make_shared<Session>(m_socket, m_remote_endpoint, m_file_path);
    m_session->start();
    
    do_receive(); 
}

void Server::do_receive() {
    auto buffer = std::make_shared<std::array<char, UdpConfig::MAX_PACKET_SIZE>>();
    // Receive from anyone, but handle_packet filters
    asio::ip::udp::endpoint sender_endpoint;
    
    // Using a member for receive_endpoint or local one? local is fine for lambda capture
    // but async_receive_from needs a reference that stays alive.
    // Let's allocation sender endpoint on heap or use member if we want.
    // However, for the lambda, we can capture a shared_ptr to an endpoint?
    auto sender_ptr = std::make_shared<asio::ip::udp::endpoint>();

    m_socket.async_receive_from(
        asio::buffer(*buffer), *sender_ptr,
        [this, buffer, sender_ptr](asio::error_code ec, std::size_t bytes_recvd) {
            if (!ec && bytes_recvd > 0) {
               std::string data(buffer->data(), bytes_recvd);
               if (m_session) {
                   m_session->handle_packet(data, *sender_ptr);
               }
            } else if (ec != asio::error::operation_aborted) {
                std::cerr << "Receive error: " << ec.message() << std::endl;
            }
            
            if (!ec || ec == asio::error::connection_reset) { // Continue on success or connection_reset (UDP ICMP)
                 do_receive();
            }
        });
}
