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
#include "v1/control.pb.h"
#include "v1/handshake.pb.h"

using asio::ip::udp;

namespace {

struct ConnectedPeer {
    udp::endpoint endpoint;
    SessionKeys keys;
};

std::vector<udp::endpoint> build_peer_candidates(const TRANSFERS& t) {
    std::vector<udp::endpoint> peers;
    peers.emplace_back(asio::ip::make_address(t.sender_ip), t.sender_port);
    if (!t.sender_local_ip.empty() && t.sender_local_port != 0) {
        peers.emplace_back(asio::ip::make_address(t.sender_local_ip),
                           t.sender_local_port);
        std::cout << "Added Sender Local Candidate: " << t.sender_local_ip
                  << ":" << t.sender_local_port << std::endl;
    }
    return peers;
}

bool recv_with_timeout(asio::io_context& io, udp::socket& socket,
                       std::array<char, UdpConfig::MAX_PACKET_SIZE>& buffer,
                       udp::endpoint& sender, std::string& data,
                       int timeout_ms) {
    bool received = false;
    asio::steady_timer timer(io, std::chrono::milliseconds(timeout_ms));
    socket.async_receive_from(asio::buffer(buffer), sender,
                              [&](asio::error_code ec, size_t len) {
                                  if (!ec) {
                                      received = true;
                                      data.assign(buffer.data(), len);
                                      timer.cancel();
                                  }
                              });
    timer.async_wait([&](auto) {
        if (!received) socket.cancel();
    });
    io.restart();
    io.run();
    return received;
}

std::string sign_client_hello(const zapshare::v1::ClientHello& hello,
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

std::optional<udp::endpoint> perform_handshake(
    asio::io_context& io, udp::socket& socket,
    const std::vector<udp::endpoint>& peers, PublicEndpoint& sender_ep,
    const std::string& token) {
    Utils::perform_udp_hole_punch(socket, sender_ep);

    std::array<char, UdpConfig::MAX_PACKET_SIZE> buf;
    udp::endpoint sender;  // Packet source

    // Handshake
    zapshare::v1::HandshakePacket handshake_packet;
    auto* hello = handshake_packet.mutable_client_hello();
    hello->set_version(zapshare::v1::PROTOCOL_VERSION_1);
    hello->set_transfer_id(token);
    hello->set_token(token);

    // TODO: need to implement
    IdentityKeyPair receiver_identity = generate_identity_keypair();
    EphemeralKeyPair receiver_ephemeral = generate_ephemeral_keypair();
    std::string receiver_nonce = random_nonce(32);
    hello->set_receiver_nonce(receiver_nonce);
    auto* identity = hello->mutable_receiver_identity();
    identity->set_ephemeral_public_key(receiver_ephemeral.public_key);
    identity->set_long_term_public_key(receiver_identity.public_key);

    hello->set_receiver_signature(sign_client_hello(*hello, receiver_identity));

    std::string bytes;
    handshake_packet.SerializeToString(&bytes);
    std::string rx;
    bool connected = false;
    udp::endpoint connected_peer;

    for (int i = 0; i < UdpConfig::MAX_RETRIES; ++i) {
        // Send HELLO to All Candidates
        for (const auto& p : peers) {
            try {
                socket.send_to(asio::buffer(bytes), p);
            } catch (...) {
            }
        }

        zapshare::v1::HandshakePacket response;
        if (recv_with_timeout(io, socket, buf, sender, rx,
                              UdpConfig::RETRY_TIMEOUT_MS) &&
            response.ParseFromString(rx) && response.has_server_hello() &&
            response.server_hello().transfer_id() == token) {
            connected = true;
            connected_peer = sender;
            break;
        }
        std::cout << "Handshake retry " << i + 1 << std::endl;
    }
    if (connected) {
        return std::move(connected_peer);
    }
    return std::nullopt;
}

std::string build_get_request(const std::string& transfer_id) {
    // Get File
    zapshare::v1::ControlPacket control_packet;
    auto* get_request = control_packet.mutable_get();

    get_request->set_transfer_id(transfer_id);

    std::string get_bytes;
    control_packet.SerializeToString(&get_bytes);
    return get_bytes;
}

bool send_ack(udp::socket& socket, const udp::endpoint& peer,
              const std::string& transfer_id, uint64_t next_offset) {
    zapshare::v1::ControlPacket ack_packet;
    auto* ack = ack_packet.mutable_ack();
    ack->set_transfer_id(transfer_id);
    ack->set_next_offset(next_offset);

    std::string ack_bytes;
    if (!ack_packet.SerializeToString(&ack_bytes)) return false;
    socket.send_to(asio::buffer(ack_bytes), peer);
    return true;
}

bool receive_file(asio::io_context& io, udp::socket& socket,
                  const udp::endpoint& peer, const std::string& transfer_id,
                  const std::string& output_filename,
                  const std::string& expected_hash,
                  const std::string& get_bytes) {
    std::ofstream out(output_filename, std::ios::binary | std::ios::trunc);
    socket.send_to(asio::buffer(get_bytes), peer);

    std::array<char, UdpConfig::MAX_PACKET_SIZE> buf;
    udp::endpoint sender;
    std::string rx;

    // Stop-and-Wait Loop
    size_t current_offset = 0;

    int retries = 0;
    while (retries < UdpConfig::MAX_RETRIES) {
        if (recv_with_timeout(io, socket, buf, sender, rx,
                              UdpConfig::RETRY_TIMEOUT_MS)) {
            if (sender != peer) {
                continue;
            }

            retries = 0;

            zapshare::v1::ControlPacket packet;
            if (!packet.ParseFromString(rx)) {
                continue;
            }
            if (packet.has_data()) {
                const auto& data = packet.data();
                if (data.transfer_id() != transfer_id) {
                    continue;
                }
                const size_t off = static_cast<size_t>(data.offset());
                const std::string& payload = data.payload();

                if (off == current_offset) {
                    out.seekp(off);
                    out.write(payload.data(),
                              static_cast<std::streamsize>(payload.size()));
                    current_offset += payload.size();
                    std::cout << "Received: " << current_offset << " bytes"
                              << std::flush;
                    send_ack(socket, peer, transfer_id, current_offset);
                } else if (off < current_offset) {
                    send_ack(socket, peer, transfer_id, current_offset);
                }
            }

            if (packet.has_done()) {
                const auto& done = packet.done();

                if (done.transfer_id() != transfer_id) {
                    continue;
                }

                if (current_offset != done.final_size()) {
                    continue;
                }

                out.flush();
                out.close();

                const std::string file_hash =
                    Crypto::compute_file_hash(output_filename);

                if (file_hash != expected_hash) {
                    std::cerr << "\nFile hash mismatch." << std::endl;
                    return false;
                }
                send_ack(socket, peer, transfer_id, current_offset);
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

            if (current_offset == 0) {
                socket.send_to(asio::buffer(get_bytes), peer);
            } else {
                send_ack(socket, peer, transfer_id, current_offset);
            }
        }
    }
    std::cerr << "\nClient timed out." << std::endl;
    return false;
}

}  // namespace

bool run_client_session(const std::string& token,
                        const std::string& output_filename) {
    asio::io_context io;
    udp::socket socket(io);
    socket.open(udp::v4());
    socket.bind(udp::endpoint(udp::v4(), 0));

    PublicEndpoint my_ep = Utils::get_public_endpoint_for_socket(io, socket);

    Utils::signal_receiver_endpoint(token, my_ep);

    TRANSFERS t = Utils::get_transfer_metadata(token);
    std::vector<udp::endpoint> peers = build_peer_candidates(t);

    PublicEndpoint sender_ep;
    sender_ep.ip = t.sender_ip;
    sender_ep.port = static_cast<uint16_t>(t.sender_port);
    sender_ep.local_ip = t.sender_local_ip;
    sender_ep.local_port = static_cast<uint16_t>(t.sender_local_port);

    auto connected_peer =
        perform_handshake(io, socket, peers, sender_ep, token);

    if (!connected_peer) {
        std::cerr << "Failed to connect to peer." << std::endl;
        return false;
    }
    std::cout << "Connected to " << connected_peer->address().to_string() << ":"
              << connected_peer->port() << std::endl;

    const std::string get_bytes = build_get_request(token);

    return receive_file(io, socket, *connected_peer, token, output_filename,
                        t.file_hash, get_bytes);
}
