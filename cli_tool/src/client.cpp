#include "client.hpp"

#include <asio.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using asio::ip::tcp;

bool run_client_session(const std::string& host, uint16_t port, const std::string& token, const std::string& output_filename) {
    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        tcp::socket socket(io);
        asio::connect(socket, resolver.resolve(host, std::to_string(port)));

        asio::streambuf buf;
        auto write_line = [&](const std::string& line) {
            std::string msg = line;
            asio::write(socket, asio::buffer(msg));
        };

        auto read_line = [&]() -> std::string {
            asio::read_until(socket, buf, "\n");
            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            return line;
        };

        // HELLO
        write_line("HELLO " + token + "\n");
        std::string resp = read_line();
        if (resp != "OK") {
            std::cerr << "Handshake failed: " << resp << std::endl;
            return false;
        }

        // GET
        write_line("GET\n");

        std::ofstream out(output_filename, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "Cannot open output file: " << output_filename << std::endl;
            return false;
        }

        while (true) {
            std::string header = read_line();
            if (header == "DONE") {
                break;
            }
            std::istringstream iss(header);
            std::string cmd, file_id;
            size_t offset = 0;
            size_t length = 0;
            iss >> cmd >> file_id >> offset >> length;
            if (cmd != "DATA" || length == 0) {
                std::cerr << "Unexpected header: " << header << std::endl;
                return false;
            }

            std::vector<char> chunk(length);
            asio::read(socket, asio::buffer(chunk.data(), length));

            out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
            out.write(chunk.data(), static_cast<std::streamsize>(length));
        }
        out.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << std::endl;
        return false;
    }
}
