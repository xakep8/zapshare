#pragma once

#include <asio.hpp>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <fstream>
#include <array>
#include <vector>

#include "types.h"
#include "utils.hpp"

using asio::ip::tcp;

enum class State { WaitingHello, Authenticated, Transferring, Closed };

class Session : public std::enable_shared_from_this<Session> {
    public:
     Session(tcp::socket socket, const std::string& file_path) : m_socket(std::move(socket)), m_file_path(file_path) {}

    void run() { wait_for_request(); }

   private:
    bool validate_token(const std::string& token) {
        try {
            m_transfer_metadata = Utils::get_transfer_metadata(token);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Token validation failed: " << e.what() << std::endl;
            return false;
        }
    }

    void send_response(const std::string& response) {
        asio::async_write(m_socket, asio::buffer(response), [this](asio::error_code ec, std::size_t) {
            if (ec) close_connection();
        });
    }

    void close_connection() {
        m_state = State::Closed;
        asio::error_code ec;
        m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        m_socket.close();
    }

    void start_chunked_transfer() {
        m_state = State::Transferring;
        m_offset = 0;
        send_next_chunk();
    }

    void send_next_chunk() {
        if (!m_file.is_open()) {
            close_connection();
            return;
        }
        m_file.read(m_chunk.data(), m_chunk.size());
        std::streamsize n = m_file.gcount();
        if (n <= 0) {
            send_response(std::string(Message::Done) + "\n");
            return;
        }
        std::string header = std::string(Message::Data) + " " + m_file_id + " " + std::to_string(m_offset) + " " + std::to_string(n) + "\n";
        std::vector<asio::const_buffer> buffers;
        buffers.push_back(asio::buffer(header));
        buffers.push_back(asio::buffer(m_chunk.data(), static_cast<size_t>(n)));
        auto self(shared_from_this());
        asio::async_write(m_socket, buffers, [this, self, n](asio::error_code ec, std::size_t) {
            if (!ec) {
                m_offset += static_cast<size_t>(n);
                send_next_chunk();
            } else {
                close_connection();
            }
        });
    }

    void handle_message(const std::string& data) {
        std::istringstream iss(data);
        std::string cmd;
        iss >> cmd;

        if (m_state == State::WaitingHello) {
            if (cmd == Message::Hello) {
                std::string token;
                iss >> token;
                if (validate_token(token)) {
                    m_state = State::Authenticated;
                    send_response("OK\n");
                } else {
                    send_response("FAIL\n");
                    close_connection();
                }
            } else {
                send_response("FAIL\n");
                close_connection();
            }
        } else if (m_state == State::Authenticated) {
            if (cmd == Message::Get) {
                // Use validated metadata and the provided file path on sender side
                m_file_id = m_transfer_metadata.id.empty() ? m_transfer_metadata.file_name : m_transfer_metadata.id;
                m_file = std::ifstream(m_file_path, std::ifstream::binary);
                if (!m_file.is_open()) {
                    send_response("FAIL\n");
                    close_connection();
                    return;
                }
                start_chunked_transfer();
            } else if (cmd == Message::Done) {
                close_connection();
            }
        } else {
            close_connection();
        }
    }

    void wait_for_request() {
        auto self(shared_from_this());
        asio::async_read_until(m_socket, m_buffer, "\n", [this, self](asio::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                std::string data{std::istreambuf_iterator<char>(&m_buffer), std::istreambuf_iterator<char>()};
                handle_message(data);
                wait_for_request();
            } else {
                std::cout << "error: " << ec << std::endl;
            }
        });
    }

   private:
    tcp::socket m_socket;
    asio::streambuf m_buffer;
    State m_state = State::WaitingHello;
    std::ifstream m_file;
    std::string m_file_id;
    std::array<char, 8192> m_chunk{};
    size_t m_offset = 0;
    TRANSFERS m_transfer_metadata{};
    std::string m_file_path;
};
