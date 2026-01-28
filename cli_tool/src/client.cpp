#include "client.hpp"

#include <asio.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "types.h"
#include "utils.hpp"

using asio::ip::udp;

bool run_client_session(const std::string& host, uint16_t port, const std::string& token, const std::string& output_filename) {
    asio::io_context io;
    udp::socket socket(io);
    socket.open(udp::v4());
    socket.bind(udp::endpoint(udp::v4(), 0));

    // STUN Logic
    try {
        udp::endpoint stun_ep = *udp::resolver(io).resolve(udp::v4(), "stun.l.google.com", "19302").begin();
        std::array<uint8_t, 20> req = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
        for(int i=8; i<20; ++i) req[i] = rand();
        socket.send_to(asio::buffer(req), stun_ep);
        
        std::array<uint8_t, 1024> buf;
        udp::endpoint sender;
        size_t n = socket.receive_from(asio::buffer(buf), sender);
        
        PublicEndpoint my_ep;
        // STUN Parsing (Short)
        for(size_t i=20; i+8<=n;) {
            uint16_t t = (buf[i]<<8)|buf[i+1], l = (buf[i+2]<<8)|buf[i+3];
            if(t==0x20 && l>=8) {
                uint32_t xk = 0x2112A442;
                uint16_t p = ((buf[i+6]<<8)|buf[i+7]) ^ (xk>>16);
                uint32_t a = ((buf[i+8]<<24)|(buf[i+9]<<16)|(buf[i+10]<<8)|buf[i+11]) ^ xk;
                my_ep.port = p;
                my_ep.ip = std::to_string(a>>24)+"."+std::to_string((a>>16)&0xFF)+"."+std::to_string((a>>8)&0xFF)+"."+std::to_string(a&0xFF);
                break;
            }
            i += 4 + l + ((4-(l%4))%4);
        }
        std::cout << "Public Endpoint: " << my_ep.ip << ":" << my_ep.port << std::endl;
        Utils::signal_receiver_endpoint(token, my_ep);
    } catch (std::exception& e) {
        std::cerr << "STUN/Signaling failed: " << e.what() << std::endl;
        // Continue anyway? NAT traversal might fail.
    }

    udp::endpoint peer(asio::ip::make_address(host), port);
    Utils::perform_udp_hole_punch(socket, {host, port});

    std::array<char, UdpConfig::MAX_PACKET_SIZE> buf;
    udp::endpoint sender;

    // Helper for Receive with Timeout
    auto recv_with_timeout = [&](std::string& data, int timeout_ms) -> bool {
        bool received = false;
        asio::steady_timer timer(io, std::chrono::milliseconds(timeout_ms));
        socket.async_receive_from(asio::buffer(buf), sender, [&](asio::error_code ec, size_t len) {
            if(!ec) { received = true; data.assign(buf.data(), len); timer.cancel(); }
        });
        timer.async_wait([&](auto){ if(!received) socket.cancel(); });
        io.restart(); io.run();
        return received;
    };

    // Handshake
    std::string hello = std::string(Message::Hello) + " " + token + "\n";
    std::string rx;
    bool connected = false;
    for(int i=0; i<UdpConfig::MAX_RETRIES; ++i) {
        socket.send_to(asio::buffer(hello), peer);
        if(recv_with_timeout(rx, UdpConfig::RETRY_TIMEOUT_MS) && rx.find(Message::Ack) == 0) {
            connected = true; break;
        }
        std::cout << "Handshake retry " << i+1 << std::endl;
    }
    if(!connected) {
        std::cerr << "Failed to connect to peer." << std::endl;
        return false;
    }
    std::cout << "Connected!" << std::endl;

    // Get File
    std::ofstream out(output_filename, std::ios::binary | std::ios::trunc);
    socket.send_to(asio::buffer(std::string(Message::Get) + "\n"), peer);
    
    // Stop-and-Wait Loop
    size_t current_offset = 0;

    int retries = 0;
    while(retries < UdpConfig::MAX_RETRIES) {
        if(recv_with_timeout(rx, UdpConfig::RETRY_TIMEOUT_MS)) {
            retries = 0; // Reset retries on success
            
            std::istringstream iss(rx);
            std::string cmd; iss >> cmd;
            if(cmd == Message::Data) {
                std::string id; size_t off, len;
                iss >> id >> off >> len;
                
                if (off == current_offset) {
                    // Correct Packet
                    int spaces=0; size_t payload_idx=0;
                    for(size_t i=0;i<rx.size();++i) {
                        if(rx[i]==' ') spaces++;
                        if(spaces==4) { payload_idx=i+1; break; }
                    }
                    
                    if(payload_idx > 0 && payload_idx + len <= rx.size()) {
                        out.seekp(off);
                        out.write(rx.data() + payload_idx, len);
                        current_offset += len;
                        std::cout << "\rReceived: " << current_offset << " bytes" << std::flush;
                        
                        // Send ACK for the NEW offset
                        socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), peer);
                    }
                } else if (off < current_offset) {
                    // Old Packet, resend ACK for current_offset to move server forward
                    socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), peer);
                }
            } else if (cmd == Message::Done) {
                // Send final ACK handling (Server might resend DONE if ACK lost)
                socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), peer); 
                std::cout << "\nTransfer Complete!" << std::endl;
                return true;
            }
        } else {
            // Timeout
            std::cout << "\rTimeout, resending ACK... " << std::flush;
            retries++;
            // Resend ACK for expected offset (Server interprets ACK(offset) as "I have received up to offset".
            // If Server sent Data(offset) and we didn't get it, we verify what we have?
            // Wait, if we timeout waiting for Data, it means we sent ACK(current_offset) and Server sent Data(current_offset) but it was lost.
            // OR Server didn't get ACK.
            // In both cases, if we resend ACK(current_offset), Server (if it has Data(current_offset)) should resend it.
            // My Session::handle_packet says: 
            // if (ack_offset == m_offset) { // Duplicate ACK ... resend_current_chunk() }
            // Correct.
            
            // Check socket.send_to(GET) initial case?
            // If we timeout on GET response. We should resend GET?
            if (current_offset == 0) {
                 socket.send_to(asio::buffer(std::string(Message::Get) + "\n"), peer);
            } else {
                 socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), peer);
            }
        }
    }
    std::cerr << "\nClient timed out." << std::endl;
    return false;
}
