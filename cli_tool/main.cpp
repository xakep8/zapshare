#include <iostream>

#include "server.hpp"
#include "types.h"
#include "utils.hpp"

void start_server() {
    asio::io_context io;
    Server s(io, 5173);
    io.run();
}

void start_client() {}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        Error::print_usage();
    }
    const std::string_view cmd = argv[1];
    if (cmd == Command::SEND) {
        if (argc < 3) {
            Error::invalid_file_path();
        }
        const std::string_view filepath = argv[2];
        if (!Utils::check_file_exists(filepath)) {
            Error::invalid_file_path();
        }
    } else if (cmd == Command::GET) {
        if (argc < 3) {
            Error::invalid_secret();
        }
        const std::string_view secret = argv[2];
        if (!Utils::look_up(secret)) {
        }
    } else {
        std::cerr << "Invalid Command!\n";
        Error::print_usage();
    }
    return 0;
}
