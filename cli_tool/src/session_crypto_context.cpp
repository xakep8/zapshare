#include "session_crypto_context.hpp"

SessionCryptoContext::SessionCryptoContext() {}

bool SessionCryptoContext::set_session_keys(const SessionKeys& keys) {
    if (keys.rx_key.empty() || keys.tx_key.empty()) {
        return false;
    }
    m_session_keys = keys;
    m_has_keys = true;
    return true;
}

bool SessionCryptoContext::set_transfer(const udp::endpoint& peer,
                                        const std::string_view& transfer_id) {
    m_transfer_id = transfer_id;
    m_peer = peer;
    return false;
}

bool SessionCryptoContext::has_keys() const { return m_has_keys; }

SessionKeys SessionCryptoContext::get_session_keys() const {
    return m_session_keys;
}

udp::endpoint SessionCryptoContext::get_peer() const { return m_peer; }

std::string SessionCryptoContext::get_transfer_id() const {
    return m_transfer_id;
}