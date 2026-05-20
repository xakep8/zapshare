#pragma once

#include <cstdint>
#include <string>

struct IdentityKeyPair {
    std::string public_key;
    std::string private_key;
};

struct EphemeralKeyPair {
    std::string public_key;
    std::string private_key;
};

struct SessionKeys {
    std::string tx_key;
    std::string rx_key;
};

IdentityKeyPair generate_identity_keypair();
EphemeralKeyPair generate_ephemeral_keypair();
std::string random_nonce(size_t size);

std::string sign(const std::string& message, const IdentityKeyPair& identity);

bool verify_signature(const std::string& message, const std::string& signature,
                      const std::string& public_key);

SessionKeys derive_client_keys(const EphemeralKeyPair& client_keypair,
                               const std::string& client_public_key);

SessionKeys derive_server_keys(const EphemeralKeyPair& server_keypair,
                               const std::string& client_public_key);

std::string encrypt_packet(const std::string& plaintext, const std::string& key,
                           uint64_t sequence);

bool decrypt_packet(const std::string& ciphertext, const std::string& key,
                    uint64_t sequence, std::string* plaintext);