cmake_minimum_required(VERSION 3.14)

project(cli_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(asio CONFIG REQUIRED)

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

add_executable(${PROJECT_NAME} src/client.cpp src/server.cpp main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE asio OpenSSL::SSL OpenSSL::Crypto)
if(TARGET zapshare_shared)
    target_link_libraries(${PROJECT_NAME} PRIVATE zapshare_shared)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE include)

set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
