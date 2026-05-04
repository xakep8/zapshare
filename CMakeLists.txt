cmake_minimum_required(VERSION 3.25)

project(zapshare VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ZAPSHARE_BUILD_CLI "Build the Zapshare CLI tool" ON)
option(ZAPSHARE_BUILD_RENDEZVOUS "Build the rendezvous server" ON)
option(ZAPSHARE_BUILD_TESTS "Build tests" ON)
option(ZAPSHARE_BUILD_PROTO "Generate and build protobuf messages" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(proto)
add_subdirectory(shared)

if(ZAPSHARE_BUILD_RENDEZVOUS)
    add_subdirectory(central_server)
endif()

if(ZAPSHARE_BUILD_CLI)
    add_subdirectory(cli_tool)
endif()

if(ZAPSHARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
