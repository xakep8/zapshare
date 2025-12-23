#include <iostream>

#include "server.hpp"
#include "types.h"
#include "utils.hpp"

void start_server() {
    asio::io_context io;
    Server s(io, 5173);
    io.run();
}

void start_client() {
    using asio::ip::tcp;
    asio::io_context io_context;

    // we need a socket and a resolver
    tcp::socket socket(io_context);
    tcp::resolver resolver(io_context);

    // now we can use connect(..)
    asio::connect(socket, resolver.resolve("127.0.0.1", "25000"));
    while (true) {
        std::string data{"some client data ..."};
        auto result = asio::write(socket, asio::buffer(data));

        // the result represents the size of the sent data
        std::cout << "data sent: " << data.length() << '/' << result << std::endl;
    }

    // and close the connection now
    asio::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        Error::print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == Command::SEND) {
        if (argc < 3) {
            Error::invalid_file_path();
            return 1;
        }
        const std::string_view filepath = argv[2];
        if (!Utils::check_file_exists(filepath)) {
            Error::invalid_file_path();
            return 1;
        }
    } else if (cmd == Command::GET) {
        if (argc < 3) {
            Error::invalid_secret();
            return 1;
        }
        const std::string_view secret = argv[2];
        if (!Utils::look_up(secret)) {
            Error::invalid_file_path();
            return 1;
        }
        Transfer_Metadata data = Utils::get_transfer_metadata(secret);
        start_client();
    } else {
        std::cerr << "Invalid Command!\n";
        Error::print_usage();
        return 1;
    }
    return 0;
}
