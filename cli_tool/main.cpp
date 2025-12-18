#include <iostream>

#include "server.hpp"
#include "types.h"
#include "utils.hpp"

void startServer() {
    asio::io_context io;
    Server s(io, 5173);
    io.run();
}

void startClient() {}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        Error::printUsage();
    }
    const std::string_view cmd = argv[1];
    if (cmd == Command::SEND) {
        if (argc < 3) {
            Error::invalidFilePath();
        }
        const std::string_view filepath = argv[2];
        if (!Utils::checkFileExists(filepath)) {
            Error::invalidFilePath();
        }
    } else if (cmd == Command::GET) {
        if (argc < 3) {
            Error::invalidSecret();
        }
        const std::string_view secret = argv[2];
        if (!Utils::lookUp(secret)) {
        }
    } else {
        std::cerr << "Invalid Command!\n";
        Error::printUsage();
    }
    return 0;
}
