#include <iostream>

#include "P2PConnection.hpp"
#include "asio.hpp"
#include "client.hpp"
#include "crypto.hpp"
#include "error.hpp"
#include "include/P2PConnection.hpp"
#include "include/utils.hpp"
#include "server.hpp"
#include "types.h"
#include "utils.hpp"

using json = nlohmann::json;

void start_server(const std::string& file_path,
                  const std::string& transfer_id) {
    asio::io_context io;
    Server s(io, 5173, file_path);
    // Server run will poll for signal and then start
    s.run(transfer_id);
    io.run();
}

int handle_get(const int argc, char* argv[]) {
    if (argc < 3) {
        Error::invalid_secret();
        return 1;
    }
    P2pConnection::Receiver rcvr;
    const std::string secret = argv[2];
    rcvr.start(secret);
    while (rcvr.getState() == P2pConnection::Receiver::State::WaitingOffer) {
        auto offer = Utils::get_offer(secret);
        if (!offer) continue;
        rcvr.acceptOffer(*offer);
        break;
    }
    if (!rcvr.waitUntil(P2pConnection::Receiver::State::MessageReceived)) {
        std::cerr << "Receiver connection failed\n";
        return 1;
    }
    // Use Sender's IP and Port (5173 or whatever registered)
    // if (!run_client_session(std::string(secret), peer_transfer.file_name)) {
    //    std::cerr << "File download failed" << std::endl;
    //    return 1;
    //}
    return 0;
}

int handle_send(const int argc, char* argv[]) {
    if (argc < 3) {
        Error::invalid_file_path();
        return 1;
    }
    // P2pConnection::Sender sender;
    // sender.send_hello();
    // return 0;
    const std::string_view filepath = argv[2];
    if (!Utils::check_file_exists(filepath)) {
        Error::invalid_file_path();
        return 1;
    }
    Transfer_t transfer{};
    transfer.file_name = std::filesystem::path(filepath).filename().string();
    transfer.file_hash = Crypto::compute_file_hash(filepath);
    transfer.file_size = std::filesystem::file_size(filepath);
    transfer.protocol = "udp";
    transfer.sender_port = 5173;
    transfer.token = Utils::generate_uuid_token();
    transfer.id = transfer.token;
    std::cout << "Your secret is: " << transfer.token << "\n";

    P2pConnection::Sender sender;
    sender.start(transfer.token);

    if (!sender.waitUntil(P2pConnection::Sender::State::OfferSent)) {
        std::cerr << "Offer creation or publishing failed\n";
        return 1;
    }

    while (sender.getState() == P2pConnection::Sender::State::OfferSent) {
        auto answer = Utils::get_answer(transfer.token);
        if (!answer) continue;
        sender.applyAnswer(*answer);
        break;
    }

    if (!sender.waitUntil(P2pConnection::Sender::State::DCOpen)) {
        std::cerr << "Sender connection failed\n";
        return 1;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        Error::print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == Command::SEND) {
        return handle_send(argc, argv);
    } else if (cmd == Command::GET) {
        return handle_get(argc, argv);
    } else {
        std::cerr << "Invalid Command!\n";
        Error::print_usage();
        return 1;
    }
    return 0;
}
