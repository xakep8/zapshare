#include "crypto/session_crypto.hpp"

#include "sodium.h"
#include "utils/check.hpp"

namespace {
void ensure_libsodium_initialized() {
    static const int initialized = sodium_init();
    if (initialized < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}
}  // namespace

IdentityKeyPair generate_identity_keypair() {
    ensure_libsodium_initialized();
    std::string public_key(crypto_sign_PUBLICKEYBYTES, '\0');
    std::string private_key(crypto_sign_SECRETKEYBYTES, '\0');
    if (crypto_sign_keypair(
            reinterpret_cast<unsigned char*>(public_key.data()),
            reinterpret_cast<unsigned char*>(private_key.data()))) {
        throw std::runtime_error("Identity Keypair generation failed");
    }
    CHECK(public_key.size() == crypto_sign_PUBLICKEYBYTES);
    CHECK(private_key.size() == crypto_sign_SECRETKEYBYTES);
    return IdentityKeyPair{public_key, private_key};
}

EphemeralKeyPair generate_ephemeral_keypair() {
    ensure_libsodium_initialized();
    std::string public_key(crypto_kx_PUBLICKEYBYTES, '\0');
    std::string private_key(crypto_kx_SECRETKEYBYTES, '\0');
    if (crypto_kx_keypair(
            reinterpret_cast<unsigned char*>(public_key.data()),
            reinterpret_cast<unsigned char*>(private_key.data()))) {
        throw std::runtime_error("Ephemeral Keypair generation failed");
    }
    CHECK(public_key.size() == crypto_kx_PUBLICKEYBYTES);
    CHECK(private_key.size() == crypto_kx_SECRETKEYBYTES);
    return EphemeralKeyPair{public_key, private_key};
}

std::string random_nonce(size_t size) {
    ensure_libsodium_initialized();
    std::string buffer(size, '\0');
    randombytes_buf(buffer.data(), buffer.size());
    return buffer;
}

std::string sign(const std::string& message, const IdentityKeyPair& identity) {
    ensure_libsodium_initialized();
    CHECK(identity.private_key.size() == crypto_sign_SECRETKEYBYTES);
    std::string signature(crypto_sign_BYTES, '\0');
    if (crypto_sign_detached(
            reinterpret_cast<unsigned char*>(signature.data()), nullptr,
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size(),
            reinterpret_cast<const unsigned char*>(
                identity.private_key.data()))) {
        throw std::runtime_error("Message signing failed");
    }
    return signature;
}

bool verify_signature(const std::string& message, const std::string& signature,
                      const std::string& public_key) {
    ensure_libsodium_initialized();
    if (signature.size() != crypto_sign_BYTES ||
        public_key.size() != crypto_sign_PUBLICKEYBYTES) {
        return false;
    }
    return crypto_sign_verify_detached(
               reinterpret_cast<const unsigned char*>(signature.data()),
               reinterpret_cast<const unsigned char*>(message.data()),
               message.size(),
               reinterpret_cast<const unsigned char*>(public_key.data())) == 0;
}

SessionKeys derive_client_keys(const EphemeralKeyPair& client_key_pair,
                               const std::string& server_public_key) {
    ensure_libsodium_initialized();
    std::string rx(crypto_kx_SESSIONKEYBYTES, '\0');
    std::string tx(crypto_kx_SESSIONKEYBYTES, '\0');
    CHECK(client_key_pair.private_key.size() == crypto_kx_SECRETKEYBYTES);
    CHECK(client_key_pair.public_key.size() == crypto_kx_PUBLICKEYBYTES);
    if (server_public_key.size() != crypto_kx_PUBLICKEYBYTES) {
        throw std::runtime_error("Invalid server public key size");
    }
    if (crypto_kx_client_session_keys(
            reinterpret_cast<unsigned char*>(rx.data()),
            reinterpret_cast<unsigned char*>(tx.data()),
            reinterpret_cast<const unsigned char*>(
                client_key_pair.public_key.data()),
            reinterpret_cast<const unsigned char*>(
                client_key_pair.private_key.data()),
            reinterpret_cast<const unsigned char*>(server_public_key.data()))) {
        throw std::runtime_error("Client session key derivation failed");
    }
    return SessionKeys{tx, rx};
}

SessionKeys derive_server_keys(const EphemeralKeyPair& server_key_pair,
                               const std::string& client_public_key) {
    ensure_libsodium_initialized();
    std::string rx(crypto_kx_SESSIONKEYBYTES, '\0');
    std::string tx(crypto_kx_SESSIONKEYBYTES, '\0');
    CHECK(server_key_pair.private_key.size() == crypto_kx_SECRETKEYBYTES);
    CHECK(server_key_pair.public_key.size() == crypto_kx_PUBLICKEYBYTES);
    if (client_public_key.size() != crypto_kx_PUBLICKEYBYTES) {
        throw std::runtime_error("Invalid client public key size");
    }
    if (crypto_kx_server_session_keys(
            reinterpret_cast<unsigned char*>(rx.data()),
            reinterpret_cast<unsigned char*>(tx.data()),
            reinterpret_cast<const unsigned char*>(
                server_key_pair.public_key.data()),
            reinterpret_cast<const unsigned char*>(
                server_key_pair.private_key.data()),
            reinterpret_cast<const unsigned char*>(client_public_key.data()))) {
        throw std::runtime_error("Server session key derivation failed");
    }
    return SessionKeys{tx, rx};
}

std::string encrypt_packet(const std::string& plaintext, const std::string& key,
                           uint64_t sequence) {
    ensure_libsodium_initialized();
    CHECK(key.size() == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    std::string nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, '\0');
    randombytes_buf(nonce.data(), nonce.size());

    std::string associated_data(reinterpret_cast<const char*>(&sequence),
                                sizeof(sequence));
    std::string ciphertext(
        plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');

    unsigned long long ciphertext_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(ciphertext.data()),
            &ciphertext_len,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            plaintext.size(),
            reinterpret_cast<const unsigned char*>(associated_data.data()),
            associated_data.size(), nullptr,
            reinterpret_cast<const unsigned char*>(nonce.data()),
            reinterpret_cast<const unsigned char*>(key.data()))) {
        throw std::runtime_error("Packet encryption failed");
    }
    ciphertext.resize(ciphertext_len);
    return nonce + ciphertext;
}

bool decrypt_packet(const std::string& ciphertext, const std::string& key,
                    uint64_t sequence, std::string* plaintext) {
    ensure_libsodium_initialized();
    CHECK(plaintext != nullptr);
    CHECK(key.size() == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    if (ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES +
                                crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return false;
    }
    std::string nonce =
        ciphertext.substr(0, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

    std::string encrypted_payload =
        ciphertext.substr(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

    if (encrypted_payload.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return false;
    }

    std::string associated_data(reinterpret_cast<const char*>(&sequence),
                                sizeof(sequence));

    std::string decrypted_payload(
        encrypted_payload.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES,
        '\0');

    unsigned long long decrypted_len = 0;

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(decrypted_payload.data()),
            &decrypted_len, nullptr,
            reinterpret_cast<const unsigned char*>(encrypted_payload.data()),
            encrypted_payload.size(),
            reinterpret_cast<const unsigned char*>(associated_data.data()),
            associated_data.size(),
            reinterpret_cast<const unsigned char*>(nonce.data()),
            reinterpret_cast<const unsigned char*>(key.data()))) {
        return false;
    }
    decrypted_payload.resize(decrypted_len);
    *plaintext = std::move(decrypted_payload);
    return true;
}