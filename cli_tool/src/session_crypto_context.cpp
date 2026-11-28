#include "session_crypto_context.hpp"

#include "utils/check.hpp"

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
    if (transfer_id.empty()) {
        return false;
    }
    m_transfer_id = transfer_id;
    m_peer = peer;
    return true;
}

bool SessionCryptoContext::has_keys() const { return m_has_keys; }

SessionKeys SessionCryptoContext::get_session_keys() const {
    return m_session_keys;
}

udp::endpoint SessionCryptoContext::get_peer() const { return m_peer; }

std::string SessionCryptoContext::get_transfer_id() const {
    return m_transfer_id;
}

std::string SessionCryptoContext::encrypt_outgoing(
    const std::string& plain_text) {
    CHECK(m_has_keys);
    return encrypt_packet(plain_text, m_session_keys.tx_key, m_send_sequence++);
}

bool SessionCryptoContext::decrypt_incoming(const std::string& cipher_text,
                                            std::string* plain_text) {
    CHECK(m_has_keys);
    if (!decrypt_packet(cipher_text, m_session_keys.rx_key, m_receive_sequence,
                        plain_text)) {
        return false;
    }
    ++m_receive_sequence;
    return true;
}
