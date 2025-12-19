cmake_minimum_required(VERSION 3.10)

project(central_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)
find_package(pqxx CONFIG QUIET)

set(PQXX_FALLBACK_INCLUDE "/opt/homebrew/opt/libpqxx/include")
find_library(PQXX_FALLBACK_LIBRARY pqxx
  HINTS "/opt/homebrew/opt/libpqxx/lib")

if(NOT pqxx_FOUND AND PQXX_FALLBACK_LIBRARY)
  # Homebrew currently installs libpqxx without a CMake package; use a manual imported target.
  add_library(pqxx::pqxx STATIC IMPORTED)
  set_target_properties(pqxx::pqxx PROPERTIES
    IMPORTED_LOCATION "${PQXX_FALLBACK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PQXX_FALLBACK_INCLUDE}")
  set(pqxx_FOUND TRUE)
endif()

add_executable(${PROJECT_NAME}
  main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(TARGET pqxx::pqxx)
  target_link_libraries(${PROJECT_NAME} PRIVATE pqxx::pqxx)
elseif(pqxx_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE pqxx)
else()
  message(FATAL_ERROR "libpqxx not found: install via Homebrew (brew install libpqxx) or set pqxx_DIR")
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION bin
)