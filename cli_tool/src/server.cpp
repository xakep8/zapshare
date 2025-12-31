#include "server.hpp"

#include <iostream>

#include "session.hpp"

Server::Server(asio::io_context& io_context, short port, const std::string& file_path)
    : m_Initialized(false), m_acceptor(io_context, tcp::endpoint(tcp::v4(), port)), m_file_path(file_path) {
    do_accept();
}

Server::~Server() { std::cout << "Destrutor\n"; }

void Server::do_accept() {
    m_Initialized = true;
    m_acceptor.async_accept([this](asio::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::cout << "Creating Session on: " << socket.remote_endpoint().address().to_string() << ":"
                      << socket.remote_endpoint().port() << "\n";
            std::make_shared<Session>(std::move(socket), m_file_path)->run();
        } else {
            std::cout << "error: " << ec.message() << std::endl;
        }
        do_accept();
    });
}
