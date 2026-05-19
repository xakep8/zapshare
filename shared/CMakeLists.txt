cmake_minimum_required(VERSION 3.14)

project(zapshare_shared LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared headers
add_library(zapshare_shared STATIC src/crypto/session_crypto.cpp)

target_include_directories(
    zapshare_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# If protobuf-generated sources exist, link them into the shared interface
if(TARGET zapshare_proto)
    target_link_libraries(zapshare_shared PUBLIC zapshare_proto)
endif()

# If OpenSSL is available, expose it to consumers (handshake crypto)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_link_libraries(zapshare_shared PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

find_package(unofficial-sodium CONFIG REQUIRED)
target_link_libraries(zapshare_shared PUBLIC unofficial-sodium::sodium)

install(
    TARGETS zapshare_shared
    EXPORT zapshareTargets
    INCLUDES DESTINATION include
)

install(
    EXPORT zapshareTargets
    NAMESPACE zapshare::
    DESTINATION lib/cmake/zapshare
)
