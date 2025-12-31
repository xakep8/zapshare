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
        std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
        asio::io_context io;
        tcp::resolver resolver(io);
        tcp::socket socket(io);
        asio::connect(socket, resolver.resolve(host, std::to_string(port)));
        std::cout << "Connected successfully" << std::endl;

        asio::streambuf buf;
        auto write_line = [&](const std::string& line) {
            std::cout << "Sending: " << line;
            asio::write(socket, asio::buffer(line));
        };

        auto read_line = [&]() -> std::string {
            std::cout << "Reading response..." << std::endl;
            asio::read_until(socket, buf, "\n");
            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            std::cout << "Received: " << line << std::endl;
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
                std::cout << "Transfer complete" << std::endl;
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

            std::cout << "Receiving chunk: offset=" << offset << " length=" << length << std::endl;
            std::vector<char> chunk(length);
            
            // First, consume any data already buffered from read_until
            size_t buffered = buf.size();
            size_t to_copy_from_buf = std::min(buffered, length);
            if (to_copy_from_buf > 0) {
                std::cout << "Copying " << to_copy_from_buf << " bytes from buffer" << std::endl;
                std::istream is(&buf);
                is.read(chunk.data(), to_copy_from_buf);
            }
            
            // Then read the rest directly from socket if needed
            size_t remaining = length - to_copy_from_buf;
            if (remaining > 0) {
                std::cout << "Reading " << remaining << " more bytes from socket" << std::endl;
                asio::read(socket, asio::buffer(chunk.data() + to_copy_from_buf, remaining));
            }

            out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
            out.write(chunk.data(), static_cast<std::streamsize>(length));
        }
        out.close();
        std::cout << "File saved: " << output_filename << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << std::endl;
        return false;
    }
}
