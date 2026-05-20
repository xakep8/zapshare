#include <cassert>
#include <iostream>
#include <string>

#include "crypto/session_crypto.hpp"

void test() {
    IdentityKeyPair identity = generate_identity_keypair();

    std::string message = "hello";
    std::string signature = sign(message, identity);

    assert(verify_signature(message, signature, identity.public_key));
    assert(!verify_signature("tampered", signature, identity.public_key));

    EphemeralKeyPair client_keys = generate_ephemeral_keypair();
    EphemeralKeyPair server_keys = generate_ephemeral_keypair();

    SessionKeys client_session =
        derive_client_keys(client_keys, server_keys.public_key);
    SessionKeys server_session =
        derive_server_keys(server_keys, client_keys.public_key);

    assert(client_session.tx_key == server_session.rx_key);
    assert(client_session.rx_key == server_session.tx_key);

    std::string plaintext = "secret packet";
    uint64_t sequence = 1;

    std::string encrypted =
        encrypt_packet(plaintext, client_session.tx_key, sequence);

    std::string decrypted;
    assert(
        decrypt_packet(encrypted, server_session.rx_key, sequence, &decrypted));
    assert(decrypted == plaintext);

    std::string wrong_sequence_plaintext;
    assert(!decrypt_packet(encrypted, server_session.rx_key, sequence + 1,
                           &wrong_sequence_plaintext));
}

int main() {
    test();
    std::cout << "Crypto tests passed\n";
    return 0;
}