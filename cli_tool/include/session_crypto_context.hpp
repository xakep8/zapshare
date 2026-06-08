#pragma once

#include <asio.hpp>
#include <string>

#include "crypto/session_crypto.hpp"

using asio::ip::udp;

class SessionCryptoContext {
   public:
    SessionCryptoContext();
    ~SessionCryptoContext() = default;
    bool set_session_keys(const SessionKeys& keys);
    bool set_transfer(const udp::endpoint& peer,
                      const std::string_view& transfer_id);

    bool has_keys() const;
    SessionKeys get_session_keys() const;
    udp::endpoint get_peer() const;
    std::string get_transfer_id() const;

   private:
    SessionKeys m_session_keys;
    bool m_has_keys = false;
    uint64_t m_send_sequence = 0ull;
    uint64_t m_receive_sequence = 0ull;
    udp::endpoint m_peer;
    std::string m_transfer_id;
};