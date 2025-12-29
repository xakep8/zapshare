cmake_minimum_required(VERSION 3.14)

project(zapshare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(ASIO_GIT_TAG "master") # or a specific tag like "asio-1-30-2"

# --- Fetch asio -------------------------------------------------------
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG        ${ASIO_GIT_TAG}
)

FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
  FetchContent_Populate(asio)

  # asio is header-only; create an INTERFACE library
  add_library(asio INTERFACE)

  target_include_directories(asio INTERFACE
    ${asio_SOURCE_DIR}/asio/include
  )

  target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
endif()
# ----------------------------------------------------------------------

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

add_executable(${PROJECT_NAME}
  src/client.cpp
  src/server.cpp
  main.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE asio OpenSSL::SSL OpenSSL::Crypto)

target_include_directories(${PROJECT_NAME} PRIVATE include)

set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION bin
)

