#pragma once

#include <asio.hpp>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <fstream>
#include <array>
#include <vector>
#include <map>
#include <chrono>

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

     bool is_closed() const { return m_state == State::Closed; }

     void handle_packet(const std::string& data, const asio::ip::udp::endpoint& sender) {
         if (sender.address() != m_remote_endpoint.address()) {
             // Just warn, but maybe update if dynamic IP handling is desired.
             // For now, keep as is.
             // m_remote_endpoint = sender; 
         }
         // Update endpoint to latest valid sender (for NAT)
         m_remote_endpoint = sender; 

         if (m_state == State::Closed) return;

         std::istringstream iss(data);
         std::string cmd;
         iss >> cmd;

         if (cmd == Message::Hello) {
             std::string token;
             iss >> token;
             if (m_state == State::WaitingHello) {
                 if (validate_token(token)) {
                     m_state = State::Authenticated;
                     send_message(std::string(Message::Ack) + "\n");
                     std::cout << "Peer Authenticated. Sent ACK." << std::endl;
                 } else {
                     send_message(std::string(Message::Error) + "\n");
                     m_state = State::Closed;
                 }
             } else if (m_state == State::Authenticated || m_state == State::Transferring) {
                 send_message(std::string(Message::Ack) + "\n");
             }
         } else if (cmd == Message::Get) {
            if (m_state == State::Authenticated) {
                 m_file_id = m_transfer_metadata.id.empty() ? m_transfer_metadata.file_name : m_transfer_metadata.id;
                 m_file = std::ifstream(m_file_path, std::ifstream::binary);
                 if (!m_file.is_open()) {
                     std::cerr << "Failed to open file: " << m_file_path << std::endl;
                     send_message(std::string(Message::Error) + "\n");
                     m_state = State::Closed;
                     return;
                 }
                 std::cout << "Starting UDP transfer (Window Size: " << m_window_size << ")..." << std::endl;
                 m_state = State::Transferring;
                 send_next_chunks();
             } else if (m_state == State::Transferring) {
                 // Resend window if client is stuck
                 check_timeouts();
             }
         } else if (cmd == Message::Ack && m_state == State::Transferring) {
             size_t ack_offset;
             if (iss >> ack_offset) {
                 handle_ack(ack_offset);
             }
         }
     }

    private:
     struct PacketInfo {
         size_t offset;
         std::string packet_data;
         std::chrono::steady_clock::time_point last_sent;
         int retries;
     };

     void send_message(const std::string& msg) {
         m_socket.send_to(asio::buffer(msg), m_remote_endpoint);
     }

     void handle_ack(size_t ack_offset) {
         // ACK acknowledges everything BEFORE ack_offset.
         // Expected offset by client is ack_offset.
         
         bool progress = false;
         
         // Remove all inflight packets that have offset < ack_offset
         // AND packet.offset + size <= ack_offset? 
         // Actually, client sends "Next Expected Offset".
         // So if client expects 1000, it means 0-999 are received.
         // So any packet with offset < ack_offset is done.
         
         auto it = m_inflight_packets.begin();
         while (it != m_inflight_packets.end()) {
             if (it->first < ack_offset) {
                 // This packet is fully received (assuming packets don't overlap strangely)
                 it = m_inflight_packets.erase(it);
                 progress = true;
             } else {
                 ++it;
             }
         }

         if (progress) {
             m_dup_ack_count = 0;
             send_next_chunks(); // Fill window
         } else {
             // Duplicate ACK for the same offset (client still waiting for ack_offset)
             if (m_inflight_packets.count(ack_offset)) {
                 m_dup_ack_count++;
                 if (m_dup_ack_count >= 3) {
                     // Fast Retransmit
                     // Verify packet exists
                     // std::cout << "Fast Retransmit: " << ack_offset << std::endl;
                     resend_packet(ack_offset);
                     m_dup_ack_count = 0;
                 }
             } else if (m_eof_reached && m_inflight_packets.empty()) {
                  // Transfer complete
                  std::cout << "Final ACK received. Transfer complete." << std::endl;
                  m_state = State::Closed;
             }
         }
     }

     void send_next_chunks() {
         while (m_inflight_packets.size() < m_window_size && !m_eof_reached) {
             send_one_chunk();
         }
     }

     void send_one_chunk() {
          if (m_eof_reached) return;

          m_file.seekg(m_next_send_offset);
          m_file.read(m_chunk_buffer.data(), UdpConfig::PAYLOAD_SIZE);
          std::streamsize bytes_read = m_file.gcount();

          if (bytes_read <= 0) {
              // Send DONE
              std::string done_msg = std::string(Message::Done) + "\n";
              send_message(done_msg);
              // Cache DONE as a special packet? Or just rely on re-sending it if we don't get final ACK?
              // The logic in handle_ack handles the cleanup.
              // We need to ensure we don't spam DONE.
              // Let's rely on Check Timeouts to resend DONE if needed.
              // But here, we mark EOF.
              m_eof_reached = true;
              m_last_packet_cache = done_msg; // Re-use this legacy member for "Last sent message"
              std::cout << "Sent DONE." << std::endl;
              return; 
          }

          std::ostringstream oss;
          oss << Message::Data << " " << m_file_id << " " << m_next_send_offset << " " << bytes_read << " ";
          std::string header = oss.str();
          
          PacketInfo info;
          info.offset = m_next_send_offset;
          info.packet_data = header;
          info.packet_data.append(m_chunk_buffer.data(), bytes_read);
          info.last_sent = std::chrono::steady_clock::now();
          info.retries = 0;

          m_socket.send_to(asio::buffer(info.packet_data), m_remote_endpoint);
          
          m_inflight_packets[m_next_send_offset] = info;
          m_next_send_offset += bytes_read;
     }

     void resend_packet(size_t offset) {
         if (m_inflight_packets.count(offset)) {
             auto& info = m_inflight_packets[offset];
             m_socket.send_to(asio::buffer(info.packet_data), m_remote_endpoint);
             info.last_sent = std::chrono::steady_clock::now();
             info.retries++;
         }
     }
     
     void check_timeouts() {
         auto now = std::chrono::steady_clock::now();
         for (auto& pair : m_inflight_packets) {
             auto& info = pair.second;
             auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - info.last_sent).count();
             if (elapsed > UdpConfig::RETRY_TIMEOUT_MS) {
                 resend_packet(info.offset);
             }
         }
         // Also resend DONE if needed (logic simplified: if inflight empty and eof reached, but state != closed?)
         // Ideally keep track of "DONE sent time"
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
     
     // Sliding Window Members
     size_t m_next_send_offset = 0;
     size_t m_window_size = 512; // Configurable window size
     std::map<size_t, PacketInfo> m_inflight_packets; // Offset -> Packet
     bool m_eof_reached = false;
     int m_dup_ack_count = 0;

     // Legacy/Unused (kept to avoid breakages if unrelated logic needs them, but can remove)
     std::string m_last_packet_cache; 
     TRANSFERS m_transfer_metadata{};
 };
