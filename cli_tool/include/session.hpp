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

using asio::ip::udp;

enum class State { WaitingHello, Authenticated, Transferring, Closed };

class Session : public std::enable_shared_from_this<Session> {
    public:
     Session(asio::ip::udp::socket& socket, asio::ip::udp::endpoint remote_endpoint, const std::string& file_path)
         : m_socket(socket), m_remote_endpoint(remote_endpoint), m_file_path(file_path) {}

     void start() {
         std::cout << "Session ready. Waiting for peer..." << std::endl;
     }

     void handle_packet(const std::string& data, const asio::ip::udp::endpoint& sender) {
         if (sender.address() != m_remote_endpoint.address()) {
             std::cerr << "Warning: Packet from different IP " << sender.address().to_string() 
                       << " (Expected: " << m_remote_endpoint.address().to_string() << "). Updating endpoint." << std::endl;
         }
         m_remote_endpoint = sender; 

         if (m_state == State::Closed) return;

         std::istringstream iss(data);
         std::string cmd;
         iss >> cmd;

         if (cmd == Message::Hello && m_state == State::WaitingHello) {
             std::string token;
             iss >> token;
             if (validate_token(token)) {
                 m_state = State::Authenticated;
                 send_message(std::string(Message::Ack) + "\n");
                 std::cout << "Peer Authenticated. Sent ACK." << std::endl;
             } else {
                 send_message(std::string(Message::Error) + "\n");
                 m_state = State::Closed;
             }
         } else if (cmd == Message::Get && m_state == State::Authenticated) {
             m_file_id = m_transfer_metadata.id.empty() ? m_transfer_metadata.file_name : m_transfer_metadata.id;
             m_file = std::ifstream(m_file_path, std::ifstream::binary);
             if (!m_file.is_open()) {
                 std::cerr << "Failed to open file: " << m_file_path << std::endl;
                 send_message(std::string(Message::Error) + "\n");
                 m_state = State::Closed;
                 return;
             }
             std::cout << "Starting UDP transfer..." << std::endl;
             m_state = State::Transferring;
             send_next_chunk();
         } else if (cmd == Message::Ack && m_state == State::Transferring) {
             size_t ack_offset;
             if (iss >> ack_offset) {
                 size_t expected_offset = m_offset + m_last_chunk_size;
                 if (ack_offset == expected_offset) {
                     // Correct ACK
                     if (m_last_chunk_done) {
                         std::cout << "Final ACK received. Transfer complete." << std::endl;
                         m_state = State::Closed;
                     } else {
                         m_offset = expected_offset;
                         send_next_chunk();
                     }
                 } else if (ack_offset == m_offset) {
                     // Duplicate ACK for previous chunk (Packet loss of Data)
                     // Resend current chunk
                     std::cout << "Duplicate ACK (" << ack_offset << "). Resending..." << std::endl;
                     resend_current_chunk();
                 } else {
                     std::cout << "Unexpected ACK offset: " << ack_offset << " (Expected: " << expected_offset << ")" << std::endl;
                 }
             }
         }
     }

    private:
     void send_message(const std::string& msg) {
         m_socket.send_to(asio::buffer(msg), m_remote_endpoint);
     }

     void resend_current_chunk() {
          if (m_last_packet_cache.empty()) {
              send_next_chunk();
          } else {
              m_socket.send_to(asio::buffer(m_last_packet_cache), m_remote_endpoint);
          }
     }

     void send_next_chunk() {
         if (!m_file.is_open()) return;

         m_file.seekg(m_offset); // Ensure we read from correct offset
         m_file.read(m_chunk_buffer.data(), UdpConfig::PAYLOAD_SIZE);
         std::streamsize bytes_read = m_file.gcount();
         
         if (bytes_read <= 0) {
             // Send DONE
             std::string done_msg = std::string(Message::Done) + "\n";
             send_message(done_msg);
             m_last_packet_cache = done_msg;
             m_last_chunk_done = true;
             m_last_chunk_size = 0;
             std::cout << "Sent DONE." << std::endl;
             return; 
         }

         m_last_chunk_size = bytes_read;
         m_last_chunk_done = false;

         std::ostringstream oss;
         oss << Message::Data << " " << m_file_id << " " << m_offset << " " << bytes_read << " ";
         std::string header = oss.str();
         
         std::string packet = header;
         packet.append(m_chunk_buffer.data(), bytes_read);

         m_last_packet_cache = packet; // Cache for retransmission
         m_socket.send_to(asio::buffer(packet), m_remote_endpoint);
     }

     bool validate_token(const std::string& token) {
         try {
             m_transfer_metadata = Utils::get_transfer_metadata(token);
             return true;
         } catch (...) {
             return false;
         }
     }

    private:
     asio::ip::udp::socket& m_socket;
     asio::ip::udp::endpoint m_remote_endpoint;
     std::string m_file_path;
     State m_state = State::WaitingHello;
     std::ifstream m_file;
     std::string m_file_id;
     std::array<char, UdpConfig::MAX_PACKET_SIZE> m_chunk_buffer;
     size_t m_offset = 0;
     size_t m_last_chunk_size = 0;
     bool m_last_chunk_done = false;
     std::string m_last_packet_cache;
     TRANSFERS m_transfer_metadata{};
};
