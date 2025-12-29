#include <openssl/sha.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace Crypto {
inline std::string generate_token() { return ""; }

inline std::string compute_file_hash(const std::string_view& path) {
    std::ifstream file(std::string(path), std::ifstream::binary);
    if (!file.is_open()) {
        throw std::runtime_error("[CRYPTO] Error opening file: ");
    }
    SHA256_CTX sha256_context;
    SHA256_Init(&sha256_context);
    const int buffer_size = 4096;
    unsigned char buffer[buffer_size];
    while (file.read(reinterpret_cast<char*>(buffer), buffer_size)) {
        SHA256_Update(&sha256_context, buffer, file.gcount());
    }

    if (file.gcount() > 0) {
        SHA256_Update(&sha256_context, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256_context);
    file.close();

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}
}  // namespace Crypto