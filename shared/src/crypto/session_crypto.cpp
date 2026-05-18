#include "crypto/session_crypto.hpp"

#include "sodium.h"

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
    int result = crypto_sign_keypair(
        reinterpret_cast<unsigned char*>(public_key.data()),
        reinterpret_cast<unsigned char*>(private_key.data()));
    if (result < 0) {
        throw std::runtime_error("Identity Keypair generation failed");
    }
    return IdentityKeyPair{public_key, private_key};
}

EphemeralKeyPair generate_ephemeral_keypair() {
    ensure_libsodium_initialized();
    std::string public_key(crypto_kx_PUBLICKEYBYTES, '\0');
    std::string private_key(crypto_kx_SECRETKEYBYTES, '\0');
    int result =
        crypto_kx_keypair(reinterpret_cast<unsigned char*>(public_key.data()),
                          reinterpret_cast<unsigned char*>(private_key.data()));
    if (result < 0) {
        throw std::runtime_error("Ephemeral Keypair generation failed");
    }
    return EphemeralKeyPair{public_key, private_key};
}

std::string random_nonce(size_t size) {
    ensure_libsodium_initialized();
    std::string buffer(size, '\0');
    randombytes_buf(buffer.data(), buffer.size());
    return buffer;
}

std::string sign(const std::string& message, const IdentityKeyPair& identity) {
    std::string signature(crypto_sign_BYTES, '\0');
    crypto_sign_detached(
        reinterpret_cast<unsigned char*>(signature.data()), nullptr,
        reinterpret_cast<const unsigned char*>(message.data()), message.size(),
        reinterpret_cast<const unsigned char*>(identity.private_key.data()));
}

bool verify_signature(const std::string& message, const std::string& signature,
                      const std::string& public_key) {
    return crypto_sign_verify_detached(
               reinterpret_cast<const unsigned char*>(signature.data()),
               reinterpret_cast<const unsigned char*>(message.data()),
               message.size(),
               reinterpret_cast<const unsigned char*>(public_key.data())) == 0;
}

SessionKeys derive_client_keys(const EphemeralKeyPair& client_keypair,
                               const std::string& server_public_key) {}

SessionKeys derive_server_keys(const EphemeralKeyPair& server_key_pair,
                               const std::string& client_public_key) {}

std::string encrypt_packet(const std::string& plaintext, const std::string& key,
                           uint64_t sequence) {}

bool decrypt_packet(const std::string& ciphertext, const std::string& key,
                    uint64_t sequence, std::string* plaintext) {}