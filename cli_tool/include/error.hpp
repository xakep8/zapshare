#include <iostream>

namespace Error {
inline void print_usage() {
    std::cerr << "usage:\n" << "    zapshare send [filepath]\n" << "    zapshare get [secret]\n";
}

inline void invalid_secret() {
    std::cerr << "Invalid secret!\n";
    print_usage();
}

inline void invalid_file_path() {
    std::cerr << "Invalid filepath!\n";
    print_usage();
}
}  // namespace Error
