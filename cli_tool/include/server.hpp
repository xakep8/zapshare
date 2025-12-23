#pragma once

#include <asio.hpp>
#include <string>

using asio::ip::tcp;

class Server {
   private:
    bool m_Initialized;
    tcp::acceptor m_acceptor;

   private:
    void do_accept();

   public:
    bool is_Initialized() const { return m_Initialized; }
    ~Server();
    Server(asio::io_context& io_context, short port);
};
