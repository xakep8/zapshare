
#include <iostream>
#include <memory>

#include "P2PConnection.hpp"
#include "client.hpp"
#include "crypto.hpp"
#include "error.hpp"
#include "include/P2PConnection.hpp"
#include "include/utils.hpp"
#include "server.hpp"
#include "session.hpp"
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
    // Need to wire Client class here and cleanup the old code.
    P2pConnection::Receiver receiver;
    const std::string secret = argv[2];
    Transfer_t transferMetadata = Utils::get_transfer_metadata(secret);
    auto client = std::make_shared<Client>(
        secret, transferMetadata.file_name,
        [&receiver](std::string_view data) { receiver.sendBytes(data); });

    receiver.onBytes(
        [weak_client = std::weak_ptr<Client>(client)](std::string bytes) {
            if (auto client = weak_client.lock()) {
                client->handle_packet(bytes);
            }
        });
    receiver.start(secret);
    while (receiver.getState() ==
           P2pConnection::Receiver::State::WaitingOffer) {
        auto offer = Utils::get_offer(secret);
        if (offer) {
            receiver.acceptOffer(*offer);
            break;
        }
    }

    if (!receiver.waitUntil(P2pConnection::Receiver::State::DCOpen)) {
        std::cerr << "Receiver connection failed\n";
        return 1;
    }

    client->start();
    client->waitUntilClosed();
}

int handle_send(const int argc, char* argv[]) {
    if (argc < 3) {
        Error::invalid_file_path();
        return 1;
    }
    const std::string_view filepath = argv[2];
    if (!Utils::check_file_exists(filepath)) {
        Error::invalid_file_path();
        return 1;
    }
    // TODO: cleanup this function to make it lean, it doesn't need all the old
    // things now.
    Transfer_t transfer{};
    transfer.file_name = std::filesystem::path(filepath).filename().string();
    transfer.file_hash = Crypto::compute_file_hash(filepath);
    transfer.file_size = std::filesystem::file_size(filepath);
    transfer.protocol = "webrtc";
    transfer.sender_port = 5173;
    transfer.token = Utils::generate_uuid_token();
    transfer.id = transfer.token;
    std::cout << "Your secret is: " << transfer.token << "\n";
    Utils::register_transfer(transfer);

    P2pConnection::Sender sender;
    auto session = std::make_shared<Session>(
        [&sender](std::string_view bytes) { sender.sendBytes(bytes); },
        std::string(filepath));

    sender.onBytes(
        [weak_session = std::weak_ptr<Session>(session)](std::string bytes) {
            if (auto session = weak_session.lock()) {
                session->handle_packet(bytes);
            }
        });
    session->start();
    sender.start(transfer.token);

    if (!sender.waitUntil(P2pConnection::Sender::State::OfferSent)) {
        std::cerr << "Offer creation or publishing failed\n";
        return 1;
    }

    while (sender.getState() == P2pConnection::Sender::State::OfferSent) {
        try {
            auto answer = Utils::get_answer(transfer.token);
            if (!answer) continue;
            sender.applyAnswer(*answer);
            break;
        } catch (const std::exception& error) {
            std::cerr << "Failed to fetch answer: " << error.what() << "\n";
            return 1;
        }
    }

    if (!sender.waitUntil(P2pConnection::Sender::State::DCOpen)) {
        std::cerr << "Sender connection failed\n";
        return 1;
    }
    session->waitUntilClosed();
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
