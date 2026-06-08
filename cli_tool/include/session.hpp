#pragma once

#include <array>
#include <asio.hpp>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "session_crypto_context.hpp"
#include "types.h"
#include "v1/control.pb.h"
#include "v1/handshake.pb.h"

using asio::ip::udp;

class Session : public std::enable_shared_from_this<Session> {
   public:
    Session(udp::socket& socket, udp::endpoint remote_endpoint,
            const std::string& file_path);

    void start();
    bool is_closed() const;
    void handle_packet(const std::string& data,
                       const asio::ip::udp::endpoint& sender);

   private:
    void send_handshake_packet(const zapshare::v1::HandshakePacket& packet);
    void send_control_packet(const zapshare::v1::ControlPacket packet);
    void send_handshake_error(zapshare::v1::ErrorCode code,
                              const std::string& message);
    void send_control_error(const std::string& transfer_id,
                            zapshare::v1::ErrorCode code,
                            const std::string& message);
    void handle_handshake_packet(const std::string& data);
    void handle_get_request(const zapshare::v1::GetRequest& get);
    void handle_ack(const zapshare::v1::Ack& ack);
    void handle_control_packet(const std::string& data);
    void send_message(const std::string& msg);
    void resend_current_chunk();
    void send_next_chunk();
    bool validate_token(const std::string& token);

   public:
    enum class ConnectionState : uint8_t {
        WaitingHello,
        Authenticated,
        Transferring,
        Closed
    };

   private:
    udp::socket& m_socket;
    udp::endpoint m_remote_endpoint;
    std::string m_file_path;
    ConnectionState m_state = ConnectionState::WaitingHello;
    std::ifstream m_file;
    std::string m_file_id;
    std::array<char, UdpConfig::MAX_PACKET_SIZE> m_chunk_buffer;
    size_t m_offset = 0;
    size_t m_last_chunk_size = 0;
    bool m_last_chunk_done = false;
    std::string m_last_packet_cache;
    TRANSFERS m_transfer_metadata{};
    SessionCryptoContext m_session_crypto;
};
