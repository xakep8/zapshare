// Client session to fetch a file from a peer using the custom protocol.
#pragma once

#include <cstdint>
#include <string>

bool run_client_session(const std::string& host, uint16_t port, const std::string& token, const std::string& output_filename);
