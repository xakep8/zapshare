#pragma once

#include <array>
#include <asio.hpp>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "crypto/session_crypto.hpp"
#include "types.h"
#include "utils.hpp"
#include "v1/control.pb.h"
#include "v1/handshake.pb.h"

using asio::ip::udp;

enum class State { WaitingHello, Authenticated, Transferring, Closed };

namespace {
std::string sign_server_hello(const zapshare::v1::ServerHello& hello,
                              const IdentityKeyPair& sender_identity) {
    std::string transcript = "";
    transcript += "server_hello";
    transcript += std::to_string(hello.version());
    transcript += hello.transfer_id();
    transcript += hello.sender_nonce();
    const auto& identity = hello.sender_identity();
    transcript += identity.long_term_public_key();
    transcript += identity.ephemeral_public_key();
    return sign(transcript, sender_identity);
}
}  // namespace

class Session : public std::enable_shared_from_this<Session> {
   public:
    Session(asio::ip::udp::socket& socket,
            asio::ip::udp::endpoint remote_endpoint,
            const std::string& file_path)
        : m_socket(socket),
          m_remote_endpoint(remote_endpoint),
          m_file_path(file_path) {}

    void start() {
        std::cout << "Session ready. Waiting for peer..." << std::endl;
    }

    bool is_closed() const { return m_state == State::Closed; }

    void send_handshake_packet(const zapshare::v1::HandshakePacket& packet) {
        std::string bytes;
        if (!packet.SerializeToString(&bytes)) {
            return;
        }

        m_socket.send_to(asio::buffer(bytes), m_remote_endpoint);
    }

    void send_control_packet(const zapshare::v1::ControlPacket packet) {
        std::string bytes;
        if (!packet.SerializeToString(&bytes)) {
            return;
        }

        m_socket.send_to(asio::buffer(bytes), m_remote_endpoint);
    }

    void send_handshake_error(zapshare::v1::ErrorCode code,
                              const std::string& message) {
        zapshare::v1::HandshakePacket packet;
        auto* error = packet.mutable_error();
        error->set_code(code);
        error->set_message(message);
        send_handshake_packet(packet);
    }

    void send_control_error(const std::string& transfer_id,
                            zapshare::v1::ErrorCode code,
                            const std::string& message) {
        zapshare::v1::ControlPacket packet;
        auto* error = packet.mutable_error();
        error->set_transfer_id(transfer_id);
        error->set_code(code);
        error->set_message(message);
        send_control_packet(packet);
    }

    void handle_handshake_packet(const std::string& data) {
        zapshare::v1::HandshakePacket packet;

        if (!packet.ParseFromString(data)) {
            return;
        }

        if (!packet.has_client_hello()) {
            send_handshake_error(zapshare::v1::ERROR_CODE_BAD_PACKET,
                                 "Expected client hello!");
            return;
        }

        const auto& hello = packet.client_hello();

        if (hello.version() != zapshare::v1::PROTOCOL_VERSION_1) {
            send_handshake_error(zapshare::v1::ERROR_CODE_BAD_PACKET,
                                 "Unsupported protocol version");
            m_state = State::Closed;
            return;
        }

        if (hello.token().empty()) {
            send_handshake_error(zapshare::v1::ERROR_CODE_INVALID_TOKEN,
                                 "Missing Token!");
            m_state = State::Closed;
            return;
        }

        if (!validate_token(hello.token())) {
            send_handshake_error(zapshare::v1::ERROR_CODE_INVALID_TOKEN,
                                 "Invalid Token");
            m_state = State::Closed;
            return;
        }

        m_state = State::Authenticated;
        zapshare::v1::HandshakePacket response;
        auto* server_hello = response.mutable_server_hello();
        server_hello->set_version(zapshare::v1::PROTOCOL_VERSION_1);
        server_hello->set_transfer_id(hello.transfer_id());

        // TODO: need to complete
        IdentityKeyPair server_identity = generate_identity_keypair();
        EphemeralKeyPair server_ephemeral = generate_ephemeral_keypair();
        std::string server_nonce = random_nonce(32);
        server_hello->set_sender_nonce(server_nonce);
        auto* identity = server_hello->mutable_sender_identity();
        identity->set_ephemeral_public_key(server_ephemeral.public_key);
        identity->set_long_term_public_key(server_identity.public_key);

        server_hello->set_sender_signature(
            sign_server_hello(*server_hello, server_identity));

        send_handshake_packet(response);
    }

    void handle_get_request(const zapshare::v1::GetRequest& get) {
        if (m_state == State::Authenticated) {
            m_file_id = m_transfer_metadata.id.empty()
                            ? m_transfer_metadata.file_name
                            : m_transfer_metadata.id;
            m_file = std::ifstream(m_file_path, std::ifstream::binary);
            if (!m_file.is_open()) {
                send_control_error(get.transfer_id(),
                                   zapshare::v1::ERROR_CODE_TRANSFER_NOT_FOUND,
                                   "Failed to open file");
                m_state = State::Closed;
                return;
            }
            std::cout << "Starting the UDP transfer...." << std::endl;
            m_state = State::Transferring;
            send_next_chunk();
            return;
        }

        if (m_state == State::Transferring) {
            resend_current_chunk();
            return;
        }
    }

    void handle_ack(const zapshare::v1::Ack& ack) {
        if (m_state != State::Transferring) {
            return;
        }

        size_t ack_offset = static_cast<size_t>(ack.next_offset());
        size_t expected_offset = m_offset + m_last_chunk_size;

        if (ack_offset == expected_offset) {
            if (m_last_chunk_done) {
                std::cout << "Final ACK received. Transfer complete"
                          << std::endl;
                m_state = State::Closed;
            } else {
                m_offset = expected_offset;
                send_next_chunk();
            }
        } else if (ack_offset == m_offset) {
            resend_current_chunk();
        }
    }

    void handle_control_packet(const std::string& data) {
        zapshare::v1::ControlPacket packet;

        if (!packet.ParseFromString(data)) {
            return;
        }

        if (packet.has_get()) {
            handle_get_request(packet.get());
            return;
        }

        if (packet.has_ack()) {
            handle_ack(packet.ack());
            return;
        }

        if (packet.has_error()) {
            m_state = State::Closed;
            return;
        }
    }

    void handle_packet(const std::string& data,
                       const asio::ip::udp::endpoint& sender) {
        if (m_state == State::Closed) return;

        if (m_state == State::WaitingHello) {
            m_remote_endpoint = sender;
            handle_handshake_packet(data);
            return;
        }

        if (m_state == State::Authenticated || m_state == State::Transferring) {
            handle_control_packet(data);
            return;
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
            m_socket.send_to(asio::buffer(m_last_packet_cache),
                             m_remote_endpoint);
        }
    }

    void send_next_chunk() {
        if (!m_file.is_open()) return;

        m_file.seekg(m_offset);  // Ensure we read from correct offset
        m_file.read(m_chunk_buffer.data(), UdpConfig::PAYLOAD_SIZE);
        std::streamsize bytes_read = m_file.gcount();

        if (bytes_read <= 0) {
            zapshare::v1::ControlPacket done_packet;
            auto* done = done_packet.mutable_done();
            done->set_transfer_id(m_transfer_metadata.id);
            done->set_final_size(m_transfer_metadata.file_size);
            done->set_file_hash(m_transfer_metadata.file_hash);
            std::string bytes;
            done_packet.SerializeToString(&bytes);
            send_message(bytes);
            m_last_packet_cache = bytes;
            m_last_chunk_done = true;
            m_last_chunk_size = 0;
            std::cout << "Sent DONE." << std::endl;
            return;
        }

        m_last_chunk_size = bytes_read;
        m_last_chunk_done = false;

        zapshare::v1::ControlPacket data_packet;
        auto* data = data_packet.mutable_data();
        data->set_transfer_id(m_transfer_metadata.id);
        data->set_offset(m_offset);
        data->set_payload(m_chunk_buffer.data(),
                          static_cast<size_t>(bytes_read));

        std::string bytes;
        data_packet.SerializeToString(&bytes);

        m_last_packet_cache = bytes;  // Cache for retransmission
        m_socket.send_to(asio::buffer(bytes), m_remote_endpoint);
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
