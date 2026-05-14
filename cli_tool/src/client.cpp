#include "client.hpp"

#include <asio.hpp>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "types.h"
#include "utils.hpp"
#include "v1/control.pb.h"
#include "v1/handshake.pb.h"

using asio::ip::udp;

namespace {

constexpr char STUN_ENDPOINT[] = "stun.l.google.com";
constexpr char STUN_PORT[] = "19302";

PublicEndpoint discover_and_signal_receiver(udp::socket& socket,
                                            asio::io_context& io,
                                            const std::string& token) {
    udp::endpoint stun_ep =
        *udp::resolver(io).resolve(udp::v4(), STUN_ENDPOINT, STUN_PORT).begin();
    std::array<uint8_t, 20> req = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
    for (int i = 8; i < 20; ++i) req[i] = rand();
    socket.send_to(asio::buffer(req), stun_ep);

    std::array<uint8_t, 1024> buf;
    udp::endpoint sender;
    size_t n = socket.receive_from(asio::buffer(buf), sender);

    PublicEndpoint my_ep;
    // Local IP/Port (from socket)
    my_ep.local_ip = socket.local_endpoint().address().to_string();
    my_ep.local_ip = Utils::get_local_ip_address();
    my_ep.local_port = socket.local_endpoint().port();

    // STUN Parsing (Short)
    bool stun_success = false;
    for (size_t i = 20; i + 8 <= n;) {
        uint16_t t = (buf[i] << 8) | buf[i + 1],
                 l = (buf[i + 2] << 8) | buf[i + 3];
        if (t == 0x20 && l >= 8) {
            uint32_t xk = 0x2112A442;
            uint16_t p = ((buf[i + 6] << 8) | buf[i + 7]) ^ (xk >> 16);
            uint32_t a = ((buf[i + 8] << 24) | (buf[i + 9] << 16) |
                          (buf[i + 10] << 8) | buf[i + 11]) ^
                         xk;
            my_ep.port = p;
            my_ep.ip = std::to_string(a >> 24) + "." +
                       std::to_string((a >> 16) & 0xFF) + "." +
                       std::to_string((a >> 8) & 0xFF) + "." +
                       std::to_string(a & 0xFF);
            stun_success = true;
            break;
        }
        i += 4 + l + ((4 - (l % 4)) % 4);
    }

    if (!stun_success) throw std::runtime_error("STUN Parse failed");

    std::cout << "Public Endpoint: " << my_ep.ip << ":" << my_ep.port
              << std::endl;
    std::cout << "Local Endpoint: " << my_ep.local_ip << ":" << my_ep.local_port
              << std::endl;

    Utils::signal_receiver_endpoint(token, my_ep);
    return my_ep;
}

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
    hello->set_receiver_nonce("");
    hello->set_receiver_signature("");
    auto* identity = hello->mutable_receiver_identity();
    identity->set_ephemeral_public_key("");
    identity->set_long_term_public_key("");

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

    // TODO: generate a uuid for the transfer and set transfer_id
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

    PublicEndpoint endpoints = discover_and_signal_receiver(socket, io, token);

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
