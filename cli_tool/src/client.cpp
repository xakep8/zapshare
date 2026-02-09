#include "client.hpp"

#include <asio.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <vector>
#include <map>

#include "types.h"
#include "utils.hpp"

using asio::ip::udp;

bool run_client_session(const std::string& host, uint16_t port, const std::string& token, const std::string& output_filename) {
    asio::io_context io;
    udp::socket socket(io);
    socket.open(udp::v4());
    socket.bind(udp::endpoint(udp::v4(), 0));

    // STUN Logic & Local IP Discovery
    try {
        udp::endpoint stun_ep = *udp::resolver(io).resolve(udp::v4(), "stun.l.google.com", "19302").begin();
        std::array<uint8_t, 20> req = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
        for(int i=8; i<20; ++i) req[i] = rand();
        socket.send_to(asio::buffer(req), stun_ep);
        
        std::array<uint8_t, 1024> buf;
        udp::endpoint sender;
        size_t n = socket.receive_from(asio::buffer(buf), sender);
        
        PublicEndpoint my_ep;
        // Local IP/Port (from socket)
        my_ep.local_ip = socket.local_endpoint().address().to_string(); // Note: if bound to 0.0.0.0 this is 0.0.0.0, need real local IP
        my_ep.local_ip = Utils::get_local_ip_address(); // Use helper
        my_ep.local_port = socket.local_endpoint().port();

        // STUN Parsing (Short)
        bool stun_success = false;
        for(size_t i=20; i+8<=n;) {
            uint16_t t = (buf[i]<<8)|buf[i+1], l = (buf[i+2]<<8)|buf[i+3];
            if(t==0x20 && l>=8) {
                uint32_t xk = 0x2112A442;
                uint16_t p = ((buf[i+6]<<8)|buf[i+7]) ^ (xk>>16);
                uint32_t a = ((buf[i+8]<<24)|(buf[i+9]<<16)|(buf[i+10]<<8)|buf[i+11]) ^ xk;
                my_ep.port = p;
                my_ep.ip = std::to_string(a>>24)+"."+std::to_string((a>>16)&0xFF)+"."+std::to_string((a>>8)&0xFF)+"."+std::to_string(a&0xFF);
                stun_success = true;
                break;
            }
            i += 4 + l + ((4-(l%4))%4);
        }
        
        if (!stun_success) throw std::runtime_error("STUN Parse failed");

        std::cout << "Public Endpoint: " << my_ep.ip << ":" << my_ep.port << std::endl;
        std::cout << "Local Endpoint: " << my_ep.local_ip << ":" << my_ep.local_port << std::endl;
        
        Utils::signal_receiver_endpoint(token, my_ep);
    } catch (std::exception& e) {
        std::cerr << "STUN/Signaling failed: " << e.what() << std::endl;
        // Continue anyway? NAT traversal might fail.
    }

    // Prepare Candidates for connection (Public + Local Sender IP)
    // We already have host/port passed in, which is Public IP/Port
    // We need Sender's Local IP/Port. 
    // To get that, we need to pass TRANSFERS struct or fetch it again?
    // main.cpp passes host/port.
    // Let's modify run_client_session to take TRANSFERS object or fetch it if needed.
    // Or simpler: fetch it inside run_client_session since we have token (id).
    // Actually we have token = secret.
    
    TRANSFERS t = Utils::get_transfer_metadata(token);
    std::vector<udp::endpoint> peers;
    peers.emplace_back(asio::ip::make_address(t.sender_ip), t.sender_port);
    if (!t.sender_local_ip.empty() && t.sender_local_port != 0) {
        peers.emplace_back(asio::ip::make_address(t.sender_local_ip), t.sender_local_port);
        std::cout << "Added Sender Local Candidate: " << t.sender_local_ip << ":" << t.sender_local_port << std::endl;
    }

    // Update hole punch to take multiple peers? 
    // We can just construct a dummy PublicEndpoint for the helper, 
    // BUT the helper is designed for ONE peer with opt local.
    // Let's manually punch here or update helper. 
    // The helper `perform_udp_hole_punch` was updated to check .local_ip of the struct.
    // So let's create a PublicEndpoint representing the Sender
    PublicEndpoint sender_ep;
    sender_ep.ip = t.sender_ip;
    sender_ep.port = static_cast<uint16_t>(t.sender_port);
    sender_ep.local_ip = t.sender_local_ip;
    sender_ep.local_port = static_cast<uint16_t>(t.sender_local_port);

    Utils::perform_udp_hole_punch(socket, sender_ep);

    std::array<char, UdpConfig::MAX_PACKET_SIZE> buf;
    udp::endpoint sender; // Packet source

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
    udp::endpoint connected_peer;

    for(int i=0; i<UdpConfig::MAX_RETRIES; ++i) {
        // Send HELLO to All Candidates
        for (const auto& p : peers) {
             try { socket.send_to(asio::buffer(hello), p); } catch(...) {}
        }
        
        if(recv_with_timeout(rx, UdpConfig::RETRY_TIMEOUT_MS) && rx.find(Message::Ack) == 0) {
            connected = true; 
            connected_peer = sender; // Lock onto the one that replied
            break;
        }
        std::cout << "Handshake retry " << i+1 << std::endl;
    }
    if(!connected) {
        std::cerr << "Failed to connect to peer." << std::endl;
        return false;
    }
    std::cout << "Connected to " << connected_peer.address().to_string() << ":" << connected_peer.port() << std::endl;

    // Get File
    std::ofstream out(output_filename, std::ios::binary | std::ios::trunc);
    socket.send_to(asio::buffer(std::string(Message::Get) + "\n"), connected_peer);
    
    // Stop-and-Wait Loop -> Sliding Window Receiver
    size_t current_offset = 0;
    std::map<size_t, std::string> packet_buffer; // Buffer for out-of-order packets


    int retries = 0;
    while(retries < UdpConfig::MAX_RETRIES) {
        if(recv_with_timeout(rx, UdpConfig::RETRY_TIMEOUT_MS)) {
            // Check if from connected_peer? or allow update?
            // Ideally should be from connected_peer.
            // But if IP check logic is loose, maybe ok.
            // Let's stick to connected_peer for sending ACKs.

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
                        // std::cout << "\rReceived: " << current_offset << " bytes" << std::flush;
                        
                        // Check Buffer for contiguous packets
                        while (packet_buffer.count(current_offset)) {
                            std::string& buffered_pkt = packet_buffer[current_offset];
                             // Parse header again to get len? Or store len?
                             // Simplified: We store raw packet payload in buffer? 
                             // No, let's store just the data to be clean, or full packet?
                             // storing full packet is easier to parse same way.
                             // But we need 'len' from it.
                             
                             // Let's parse the buffered packet
                             std::istringstream b_iss(buffered_pkt);
                             std::string b_cmd, b_id; size_t b_off, b_len;
                             b_iss >> b_cmd >> b_id >> b_off >> b_len;
                             
                             int b_spaces=0; size_t b_payload_idx=0;
                             for(size_t i=0;i<buffered_pkt.size();++i) {
                                 if(buffered_pkt[i]==' ') b_spaces++;
                                 if(b_spaces==4) { b_payload_idx=i+1; break; }
                             }
                             
                             out.seekp(b_off);
                             out.write(buffered_pkt.data() + b_payload_idx, b_len);
                             current_offset += b_len;
                             packet_buffer.erase(b_off);
                        }

                        
                        double percent = (static_cast<double>(current_offset) / t.file_size) * 100.0;
                        std::cout << "\rReceived: " << current_offset << " / " << t.file_size << " bytes (" << std::fixed << std::setprecision(1) << percent << "%)" << std::flush;

                        // Send ACK for the NEW offset
                        socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), connected_peer);
                    }
                } else if (off < current_offset) {
                    // Old Packet, resend ACK
                    socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), connected_peer);
                } else {
                     // Future packet (Out of Order). Buffer it.
                     if (packet_buffer.find(off) == packet_buffer.end()) {
                         packet_buffer[off] = rx;
                     }
                     // Send ACK for current_offset (Duplicate ACK) to trigger fast retransmit for the GAP
                     socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), connected_peer);
                }
            } else if (cmd == Message::Done) {
                // Send final ACK handling (Server might resend DONE if ACK lost)
                socket.send_to(asio::buffer(std::string(Message::Ack) + " " + std::to_string(current_offset) + "\n"), connected_peer); 
                std::cout << "\nTransfer Complete!" << std::endl;
                return true;
            }
        } else {
            // Timeout
            std::cout << "\rTimeout, resending ACK... " << std::flush;
            retries++;
            
            if (current_offset == 0) {
                 socket.send_to(asio::buffer(std::string(Message::Get) + "\n"), connected_peer);
            } else {
                 // Send GET to trigger "check_timeouts" on Sender side immediately
                 // effectively synonymous to a NACK for the window
                 socket.send_to(asio::buffer(std::string(Message::Get) + "\n"), connected_peer);
            }
        }
    }
    std::cerr << "\nClient timed out." << std::endl;
    return false;
}
