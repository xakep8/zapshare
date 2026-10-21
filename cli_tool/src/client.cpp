#include "client.hpp"

#include <asio.hpp>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "crypto/session_crypto.hpp"
#include "types.h"
#include "utils.hpp"
#include "v1/common.pb.h"
#include "v1/control.pb.h"
#include "v1/handshake.pb.h"

using asio::ip::udp;

namespace {

std::vector<udp::endpoint> build_peer_candidates(const Transfer_t& t) {
    std::vector<udp::endpoint> peers;
    peers.emplace_back(asio::ip::make_address(t.sender_ip), t.sender_port);
    if (!t.sender_local_ip.empty() && t.sender_local_port != 0) {
        peers.emplace_back(asio::ip::make_address(t.sender_local_ip),
                           t.sender_local_port);
        std::cout << "Added Sender Local Candidate: " << t.sender_local_ip
                  << ":" << t.sender_local_port << std::endl;
    }
    return std::move(peers);
}

std::string sign_client_hello_transcript(
    const zapshare::v1::ClientHello& hello,
    const IdentityKeyPair& receiver_identity) {
    std::string transcript = "";
    transcript += "client_hello";
    transcript += std::to_string(hello.version());
    transcript += hello.transfer_id();
    transcript += hello.token();
    transcript += hello.receiver_nonce();
    const auto& identity = hello.receiver_identity();
    transcript += identity.long_term_public_key();
    transcript += identity.ephemeral_public_key();
    return sign(transcript, receiver_identity);
}

std::string build_server_hello_transcript(
    const zapshare::v1::ServerHello& hello) {
    std::string transcript;
    transcript += "server_hello";
    transcript += std::to_string(hello.version());
    transcript += hello.transfer_id();
    transcript += hello.sender_nonce();

    const auto& identity = hello.sender_identity();
    transcript += identity.long_term_public_key();
    transcript += identity.ephemeral_public_key();

    return transcript;
}

bool validate_server_hello(const zapshare::v1::ServerHello& server_hello,
                           const std::string& token) {
    if (server_hello.version() != zapshare::v1::PROTOCOL_VERSION_1)
        return false;
    if (server_hello.transfer_id() != token) return false;

    if (server_hello.sender_nonce().empty()) return false;

    if (server_hello.sender_identity().ephemeral_public_key().empty())
        return false;

    if (server_hello.sender_identity().long_term_public_key().empty())
        return false;

    if (server_hello.sender_signature().empty()) return false;

    return true;
}

}  // namespace

Client::Client()
    : m_token(), m_output_filename(), m_context(), m_socket(m_context) {}

Client::Client(const std::string& token, const std::string& output_filename)
    : m_context(), m_socket(m_context) {
    m_token = token;
    m_output_filename = output_filename;
}

bool Client::run() {
    if (!discover_and_signal_receiver()) {
        std::cerr << "Failed to Signal server" << std::endl;
        return false;
    }
    if (!perform_handshake()) {
        std::cerr << "Failed to connect to peer." << std::endl;
        return false;
    }
    std::cout << "Connected to "
              << m_connected_peer->endpoint.address().to_string() << ":"
              << m_connected_peer->endpoint.port() << std::endl;

    if (!send_get_request()) {
        std::cerr << "Get Request Send Failed!\n";
        return false;
    }

    return receive_file(m_connected_peer->endpoint);
}

std::string Client::build_get_request() const {
    // Get File
    zapshare::v1::ControlPacket control_packet;
    auto* get_request = control_packet.mutable_get();

    get_request->set_transfer_id(m_token);

    std::string get_bytes;
    control_packet.SerializeToString(&get_bytes);
    return get_bytes;
}

std::string Client::build_ack(uint64_t next_offset) const {
    zapshare::v1::ControlPacket ack_packet;
    auto* ack = ack_packet.mutable_ack();
    ack->set_transfer_id(m_transfer_data.id);
    ack->set_next_offset(next_offset);

    std::string bytes;
    if (!ack_packet.SerializeToString(&bytes)) {
        std::cerr << "SerializeToString failed!\n";
        return "";
    }
    return bytes;
}

bool Client::send_get_request() {
    const std::string get_request = build_get_request();
    const std::string encrypted_request =
        m_crypto.encrypt_outgoing(get_request);
    m_socket.send_to(asio::buffer(encrypted_request),
                     m_connected_peer->endpoint);
    return true;
}

bool Client::discover_and_signal_receiver() {
    m_socket.open(udp::v4());
    m_socket.bind(udp::endpoint(udp::v4(), 0));

    PublicEndpoint my_ep =
        Utils::get_public_endpoint_for_socket(m_context, m_socket);

    Utils::signal_receiver_endpoint(m_token, my_ep);

    m_transfer_data = Utils::get_transfer_metadata(m_token);

    m_peer_candidates = build_peer_candidates(m_transfer_data);

    m_sender_ep.ip = m_transfer_data.sender_ip;
    m_sender_ep.port = static_cast<uint16_t>(m_transfer_data.sender_port);
    m_sender_ep.local_ip = m_transfer_data.sender_local_ip;
    m_sender_ep.local_port =
        static_cast<uint16_t>(m_transfer_data.sender_local_port);

    return true;
}

bool Client::send_ack(uint64_t next_offset) {
    std::string ack_bytes = build_ack(next_offset);
    std::string encrypted_request = m_crypto.encrypt_outgoing(ack_bytes);
    m_socket.send_to(asio::buffer(encrypted_request),
                     m_connected_peer->endpoint);
    return true;
}

bool Client::receive_file(const udp::endpoint& endpoint) {
    std::ofstream out(m_output_filename, std::ios::binary | std::ios::trunc);

    std::array<char, UdpConfig::MAX_PACKET_SIZE> buf;
    udp::endpoint sender;
    std::string rx;

    int retries = 0;
    while (retries < UdpConfig::MAX_RETRIES) {
        if (recv_with_timeout(sender, rx)) {
            if (sender != m_connected_peer->endpoint) {
                continue;
            }

            retries = 0;

            std::string decrypted_request{};
            if (!m_crypto.decrypt_incoming(rx, &decrypted_request)) {
                continue;
            }
            zapshare::v1::ControlPacket packet;
            if (!packet.ParseFromString(decrypted_request)) {
                continue;
            }
            if (packet.has_data()) {
                const auto& data = packet.data();
                if (data.transfer_id() != m_transfer_data.id) {
                    continue;
                }
                const size_t off = static_cast<size_t>(data.offset());
                const std::string& payload = data.payload();

                if (off == m_current_offset) {
                    out.seekp(off);
                    out.write(payload.data(),
                              static_cast<std::streamsize>(payload.size()));
                    m_current_offset += payload.size();
                    std::cout << "Received: " << m_current_offset << " bytes"
                              << std::flush;
                    send_ack(m_current_offset);
                } else if (off < m_current_offset) {
                    send_ack(m_current_offset);
                }
            }

            if (packet.has_done()) {
                const auto& done = packet.done();

                if (done.transfer_id() != m_transfer_data.id) {
                    continue;
                }

                if (m_current_offset != done.final_size()) {
                    continue;
                }

                out.flush();
                out.close();

                const std::string file_hash =
                    Crypto::compute_file_hash(m_output_filename);

                if (file_hash != m_transfer_data.file_hash) {
                    std::cerr << "\nFile hash mismatch." << std::endl;
                    return false;
                }
                send_ack(m_current_offset);
                std::cout << "\nTransfer Complete!" << std::endl;
                return true;
            }

            if (packet.has_error()) {
                std::cerr << "Peer returned error: " << packet.error().message()
                          << std::endl;
                return false;
            }
        } else {
            // Timeout
            std::cout << "\rTimeout, resending ACK... " << std::flush;
            retries++;

            if (m_current_offset == 0) {
                send_get_request();
            } else {
                send_ack(m_current_offset);
            }
        }
    }
    std::cerr << "\nClient timed out." << std::endl;
    return false;
}

bool Client::perform_handshake() {
    Utils::perform_udp_hole_punch(m_socket, m_sender_ep);

    std::array<char, UdpConfig::MAX_PACKET_SIZE> buf;
    udp::endpoint sender;  // Packet source

    // Handshake
    zapshare::v1::HandshakePacket handshake_packet;
    auto* hello = handshake_packet.mutable_client_hello();
    hello->set_version(zapshare::v1::PROTOCOL_VERSION_1);
    hello->set_transfer_id(m_token);
    hello->set_token(m_token);

    IdentityKeyPair receiver_identity = generate_identity_keypair();
    EphemeralKeyPair receiver_ephemeral = generate_ephemeral_keypair();
    std::string receiver_nonce = random_nonce(32);
    hello->set_receiver_nonce(receiver_nonce);
    auto* identity = hello->mutable_receiver_identity();
    identity->set_ephemeral_public_key(receiver_ephemeral.public_key);
    identity->set_long_term_public_key(receiver_identity.public_key);

    hello->set_receiver_signature(
        sign_client_hello_transcript(*hello, receiver_identity));

    std::string bytes;
    handshake_packet.SerializeToString(&bytes);
    std::string rx;
    bool connected = false;
    udp::endpoint connected_peer;
    SessionKeys keys;

    for (int i = 0; i < UdpConfig::MAX_RETRIES; ++i) {
        // Send HELLO to All Candidates
        for (const auto& p : m_peer_candidates) {
            try {
                m_socket.send_to(asio::buffer(bytes), p);
            } catch (...) {
            }
        }

        zapshare::v1::HandshakePacket response;
        if (recv_with_timeout(sender, rx) && response.ParseFromString(rx) &&
            response.has_server_hello()) {
            const auto& server_hello = response.server_hello();

            if (!validate_server_hello(server_hello, m_token)) {
                continue;
            }

            const std::string transcript =
                build_server_hello_transcript(server_hello);

            if (!verify_signature(
                    transcript, server_hello.sender_signature(),
                    server_hello.sender_identity().long_term_public_key())) {
                continue;
            }

            connected = true;
            connected_peer = sender;
            keys = derive_client_keys(
                receiver_ephemeral,
                server_hello.sender_identity().ephemeral_public_key());
            m_crypto.set_session_keys(keys);
            break;
        }
        std::cout << "Handshake retry " << i + 1 << std::endl;
    }
    if (connected) {
        m_connected_peer =
            ConnectedPeer{.endpoint = connected_peer, .keys = keys};
        return true;
    }
    m_connected_peer = std::nullopt;
    return false;
}

bool run_client_session(const std::string& token,
                        const std::string& output_filename) {
    Client client(token, output_filename);
    return client.run();
}

bool Client::recv_with_timeout(udp::endpoint& sender, std::string& data) {
    bool received = false;
    asio::steady_timer timer(
        m_context, std::chrono::milliseconds(UdpConfig::RETRY_TIMEOUT_MS));
    m_socket.async_receive_from(asio::buffer(m_buffer), sender,
                                [&](asio::error_code ec, size_t len) {
                                    if (!ec) {
                                        received = true;
                                        data.assign(m_buffer.data(), len);
                                        timer.cancel();
                                    }
                                });
    timer.async_wait([&](auto) {
        if (!received) m_socket.cancel();
    });
    m_context.restart();
    m_context.run();
    return received;
}
