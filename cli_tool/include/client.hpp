#pragma once

#include <array>
#include <asio.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "crypto/session_crypto.hpp"
#include "session_crypto_context.hpp"
#include "types.h"
#include "utils.hpp"

using asio::ip::udp;

class Client {
   public:
    struct ConnectedPeer {
        udp::endpoint endpoint;
        SessionKeys keys;
    };

    Client();

    explicit Client(const std::string& token,
                    const std::string& output_filename);

    ~Client() = default;

    bool run();

   private:
    bool perform_handshake();
    bool send_get_request();
    bool discover_and_signal_receiver();
    bool receive_file(const udp::endpoint& endpoint);

    bool recv_with_timeout(asio::ip::udp::endpoint& sender, std::string& data);
    bool send_ack(uint64_t next_offset);

    std::string build_get_request() const;
    std::string build_ack(uint64_t next_offset) const;

    asio::io_context m_context;
    asio::ip::udp::socket m_socket;
    std::array<char, UdpConfig::MAX_PACKET_SIZE> m_buffer;
    std::string m_token;
    std::string m_output_filename;

    PublicEndpoint m_sender_ep;
    Transfer_t m_transfer_data;
    std::vector<asio::ip::udp::endpoint> m_peer_candidates;
    std::optional<ConnectedPeer> m_connected_peer;
    SessionCryptoContext m_crypto;

    uint64_t m_current_offset = 0;
    std::string m_cached_get_packet;
};

bool run_client_session(const std::string& token,
                        const std::string& output_filename);
