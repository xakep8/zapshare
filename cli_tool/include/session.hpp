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

    void send_response(const std::string& response, bool close_after = false) {
        auto self(shared_from_this());
        asio::async_write(m_socket, asio::buffer(response), [this, self, close_after](asio::error_code ec, std::size_t) {
            if (ec || close_after) close_connection();
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
        std::cout << "Starting transfer from file: " << m_file_path << std::endl;
        send_next_chunk();
    }

    void send_next_chunk() {
        if (!m_file.is_open()) {
            std::cerr << "File not open for reading: " << m_file_path << std::endl;
            close_connection();
            return;
        }
        std::cout << "Attempting to read from file at offset: " << m_offset << std::endl;
        m_file.read(m_chunk.data(), m_chunk.size());
        std::streamsize n = m_file.gcount();
        std::cout << "Read " << n << " bytes from file" << std::endl;
        if (n <= 0) {
            std::cout << "No more data to read, sending DONE" << std::endl;
            send_response(std::string(Message::Done) + "\n", true);
            return;
        }
        std::cout << "Preparing to send chunk: offset=" << m_offset << " length=" << n << std::endl;
        m_header = std::string(Message::Data) + " " + m_file_id + " " + std::to_string(m_offset) + " " + std::to_string(n) + "\n";
        std::cout << "Header: " << m_header;
        std::vector<asio::const_buffer> buffers;
        buffers.push_back(asio::buffer(m_header));
        buffers.push_back(asio::buffer(m_chunk.data(), static_cast<size_t>(n)));
        std::cout << "Initiating async_write for header (" << m_header.size() << " bytes) + data (" << n << " bytes)" << std::endl;
        auto self(shared_from_this());
        asio::async_write(m_socket, buffers, [this, self, n](asio::error_code ec, std::size_t bytes_written) {
            if (!ec) {
                std::cout << "async_write completed successfully, wrote " << bytes_written << " bytes" << std::endl;
                m_offset += static_cast<size_t>(n);
                send_next_chunk();
            } else {
                std::cerr << "async_write failed: " << ec.message() << std::endl;
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
                    send_response("FAIL\n", true);
                    close_connection();
                }
            } else {
                send_response("FAIL\n", true);
                close_connection();
            }
        } else if (m_state == State::Authenticated) {
            if (cmd == Message::Get) {
                // Use validated metadata and the provided file path on sender side
                m_file_id = m_transfer_metadata.id.empty() ? m_transfer_metadata.file_name : m_transfer_metadata.id;
                m_file = std::ifstream(m_file_path, std::ifstream::binary);
                if (!m_file.is_open()) {
                    std::cerr << "Failed to open file for GET: " << m_file_path << std::endl;
                    send_response("FAIL\n", true);
                    close_connection();
                    return;
                }
                std::cout << "GET received, starting transfer of " << m_file_path << std::endl;
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
                std::istream is(&m_buffer);
                std::string line;
                std::getline(is, line);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();  // Remove CR if CRLF
                }
                std::cout << "Session received: " << line << std::endl;
                handle_message(line);
                if (m_state != State::Closed && m_state != State::Transferring) {
                    wait_for_request();
                }
            } else {
                std::cerr << "Session read error: " << ec.category().name() << ":" << ec.value() << " (" << ec.message() << ")" << std::endl;
                close_connection();
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
    std::string m_header;  // Keep header alive for async_write
};
