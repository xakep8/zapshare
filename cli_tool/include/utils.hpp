#pragma once

#include <filesystem>
#include <iostream>

namespace Error {
void print_usage() { std::cerr << "usage:\n" << "    zapshare send [filepath]\n" << "    zapshare get [secret]\n"; }

void invalid_secret() {
    std::cerr << "Invalid secret!\n";
    print_usage();
}

void invalid_file_path() {
    std::cerr << "Invalid filepath!\n";
    print_usage();
}
}  // namespace Error

namespace Utils {
bool check_file_exists(const std::string_view filepath) {
    std::filesystem::path file{filepath};
    return std::filesystem::exists(file);
}

bool look_up(const std::string_view secret) { return false; }
}  // namespace Utils