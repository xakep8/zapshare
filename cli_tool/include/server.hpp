#pragma once

#include <asio.hpp>
#include <string>

using asio::ip::tcp;

class Server {
   private:
    bool m_Initialized;
    asio::ip::udp::socket m_socket;
    std::string m_file_path;
    asio::ip::udp::endpoint m_remote_endpoint;

   private:
    void do_receive();

   public:
    bool is_Initialized() const { return m_Initialized; }
    ~Server();
    Server(asio::io_context& io_context, short port, const std::string& file_path);
    void run(const std::string& transfer_id);
    
    private:
     std::shared_ptr<class Session> m_session;
};
