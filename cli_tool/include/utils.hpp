#pragma once

#include <filesystem>
#include <iostream>

namespace Error {
void printUsage() { std::cerr << "usage:\n" << "    zapshare send [filepath]\n" << "    zapshare get [secret]\n"; }

void invalidSecret() {
    std::cerr << "Invalid secret!\n";
    printUsage();
}

void invalidFilePath() {
    std::cerr << "Invalid filepath!\n";
    printUsage();
}
}  // namespace Error

namespace Utils {
bool checkFileExists(const std::string_view filepath) {
    std::filesystem::path file{filepath};
    return std::filesystem::exists(file);
}

bool lookUp(const std::string_view secret) { return false; }
}  // namespace Utils