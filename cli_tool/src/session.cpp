#include "session.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "crypto/session_crypto.hpp"
#include "utils.hpp"

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

std::string build_client_hello_transcript(
    const zapshare::v1::ClientHello& hello) {
    std::string transcript = "";
    transcript += "client_hello";
    transcript += std::to_string(hello.version());
    transcript += hello.transfer_id();
    transcript += hello.token();
    transcript += hello.receiver_nonce();
    const auto& identity = hello.receiver_identity();
    transcript += identity.long_term_public_key();
    transcript += identity.ephemeral_public_key();
    return transcript;
}

bool validate_client_hello(const zapshare::v1::ClientHello& hello) {
    if (hello.version() != zapshare::v1::PROTOCOL_VERSION_1) return false;

    if (hello.transfer_id().empty()) return false;

    if (hello.token().empty()) return false;

    if (hello.receiver_nonce().empty()) return false;

    if (hello.receiver_signature().empty()) return false;

    if (hello.receiver_identity().ephemeral_public_key().empty()) return false;

    if (hello.receiver_identity().long_term_public_key().empty()) return false;

    return verify_signature(build_client_hello_transcript(hello),
                            hello.receiver_signature(),
                            hello.receiver_identity().long_term_public_key());
}
}  // namespace

Session::Session(SendBytes sendBytes, const std::string& file_path)
    : m_sendBytes(std::move(sendBytes)), m_file_path(file_path) {
    if (!m_sendBytes) {
        throw std::invalid_argument("Session requires a send function!\n");
    }
}

void Session::start() {
    std::cout << "Session ready. Waiting for peer..." << std::endl;
}

bool Session::is_closed() const {
    const auto state = getState();
    return state == ConnectionState::Closed;
}

void Session::handle_packet(const std::string& data) {
    const auto state = getState();
    if (state == ConnectionState::Closed) return;

    if (state == ConnectionState::WaitingHello) {
        handle_handshake_packet(data);
        return;
    }

    if (state == ConnectionState::Authenticated ||
        state == ConnectionState::Transferring) {
        handle_control_packet(data);
    }
}

// private functions

void Session::send_handshake_packet(
    const zapshare::v1::HandshakePacket& packet) {
    std::string bytes;
    if (!packet.SerializeToString(&bytes)) {
        return;
    }

    m_sendBytes(bytes);
}

void Session::send_control_packet(const zapshare::v1::ControlPacket& packet) {
    std::string bytes;
    if (!packet.SerializeToString(&bytes)) {
        return;
    }
    std::string encrypted_control_pkt =
        m_session_crypto.encrypt_outgoing(bytes);
    m_sendBytes(encrypted_control_pkt);
}

void Session::send_handshake_error(zapshare::v1::ErrorCode code,
                                   const std::string& message) {
    zapshare::v1::HandshakePacket packet;
    auto* error = packet.mutable_error();
    error->set_code(code);
    error->set_message(message);
    send_handshake_packet(packet);
}

void Session::send_control_error(const std::string& transfer_id,
                                 zapshare::v1::ErrorCode code,
                                 const std::string& message) {
    zapshare::v1::ControlPacket packet;
    auto* error = packet.mutable_error();
    error->set_transfer_id(transfer_id);
    error->set_code(code);
    error->set_message(message);
    send_control_packet(packet);
}

void Session::handle_handshake_packet(const std::string& data) {
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
        setState(ConnectionState::Closed);
        return;
    }

    if (hello.token().empty()) {
        send_handshake_error(zapshare::v1::ERROR_CODE_INVALID_TOKEN,
                             "Missing Token!");
        setState(ConnectionState::Closed);
        return;
    }

    if (!validate_token(hello.token())) {
        send_handshake_error(zapshare::v1::ERROR_CODE_INVALID_TOKEN,
                             "Invalid Token");
        setState(ConnectionState::Closed);
        return;
    }

    if (!validate_client_hello(hello)) {
        send_handshake_error(zapshare::v1::ERROR_CODE_UNAUTHORIZED_PEER,
                             "Invalid Packet!");
        setState(ConnectionState::Closed);
        return;
    }

    setState(ConnectionState::Authenticated);
    zapshare::v1::HandshakePacket response;
    auto* server_hello = response.mutable_server_hello();
    server_hello->set_version(zapshare::v1::PROTOCOL_VERSION_1);
    server_hello->set_transfer_id(hello.transfer_id());

    IdentityKeyPair server_identity = generate_identity_keypair();
    EphemeralKeyPair server_ephemeral = generate_ephemeral_keypair();

    SessionKeys keys = derive_server_keys(
        server_ephemeral, hello.receiver_identity().ephemeral_public_key());
    m_session_crypto.set_session_keys(std::move(keys));

    std::string server_nonce = random_nonce(32);
    server_hello->set_sender_nonce(server_nonce);

    auto* identity = server_hello->mutable_sender_identity();
    identity->set_ephemeral_public_key(server_ephemeral.public_key);
    identity->set_long_term_public_key(server_identity.public_key);

    server_hello->set_sender_signature(
        sign_server_hello(*server_hello, server_identity));

    send_handshake_packet(response);
}

void Session::handle_get_request(const zapshare::v1::GetRequest& get) {
    const auto state = getState();
    if (state == ConnectionState::Authenticated) {
        m_file_id = m_transfer_metadata.id.empty()
                        ? m_transfer_metadata.file_name
                        : m_transfer_metadata.id;
        m_file = std::ifstream(m_file_path, std::ifstream::binary);
        if (!m_file.is_open()) {
            send_control_error(get.transfer_id(),
                               zapshare::v1::ERROR_CODE_TRANSFER_NOT_FOUND,
                               "Failed to open file");
            setState(ConnectionState::Closed);
            return;
        }
        std::cout << "Starting the UDP transfer...." << std::endl;
        setState(ConnectionState::Transferring);
        send_next_chunk();
        return;
    }

    if (state == ConnectionState::Transferring) {
        resend_current_chunk();
        return;
    }
}

void Session::handle_ack(const zapshare::v1::Ack& ack) {
    auto state = getState();
    if (state != ConnectionState::Transferring) {
        return;
    }

    size_t ack_offset = static_cast<size_t>(ack.next_offset());
    size_t expected_offset = m_offset + m_last_chunk_size;

    if (ack_offset == expected_offset) {
        if (m_last_chunk_done) {
            std::cout << "Final ACK received. Transfer complete" << std::endl;
            setState(ConnectionState::Closed);
        } else {
            m_offset = expected_offset;
            send_next_chunk();
        }
    } else if (ack_offset == m_offset) {
        resend_current_chunk();
    }
}

void Session::handle_control_packet(const std::string& data) {
    zapshare::v1::ControlPacket packet;

    std::string decrypted_control{};
    if (!m_session_crypto.decrypt_incoming(data, &decrypted_control)) {
        return;
    }

    if (!packet.ParseFromString(decrypted_control)) {
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
        setState(ConnectionState::Closed);
        return;
    }
}

void Session::send_raw_bytes(const std::string& msg) { m_sendBytes(msg); }

void Session::resend_current_chunk() {
    if (m_last_packet_cache.empty()) {
        send_next_chunk();
    } else {
        m_sendBytes(m_last_packet_cache);
    }
}

void Session::send_next_chunk() {
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
        std::string encrypted_bytes = m_session_crypto.encrypt_outgoing(bytes);
        send_raw_bytes(encrypted_bytes);
        m_last_packet_cache = encrypted_bytes;
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
    data->set_payload(m_chunk_buffer.data(), static_cast<size_t>(bytes_read));

    std::string bytes;
    data_packet.SerializeToString(&bytes);
    std::string encrypted_bytes = m_session_crypto.encrypt_outgoing(bytes);

    m_last_packet_cache = encrypted_bytes;  // Cache for retransmission
    send_raw_bytes(encrypted_bytes);
}

bool Session::validate_token(const std::string& token) {
    try {
        m_transfer_metadata = Utils::get_transfer_metadata(token);
        return true;
    } catch (...) {
        return false;
    }
}

Session::ConnectionState Session::getState() const {
    return m_state.load(std::memory_order_acquire);
}

void Session::setState(ConnectionState state) {
    m_state.store(state, std::memory_order_release);
    m_state.notify_all();
}

void Session::waitUntilClosed() const {
    auto current = getState();

    while (current != ConnectionState::Closed) {
        m_state.wait(current, std::memory_order_acquire);
        current = getState();
    }
}
