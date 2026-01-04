cmake_minimum_required(VERSION 3.10)

project(central_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)
find_package(pqxx CONFIG QUIET)

# Platform-specific libpqxx handling
if(NOT pqxx_FOUND)
  if(APPLE)
    # macOS with Homebrew
    set(HOMEBREW_PREFIX "/opt/homebrew")
    find_library(PQXX_LIBRARY pqxx
      HINTS "${HOMEBREW_PREFIX}/opt/libpqxx/lib")
    find_path(PQXX_INCLUDE_DIR pqxx/pqxx
      HINTS "${HOMEBREW_PREFIX}/opt/libpqxx/include")

    if(PQXX_LIBRARY AND PQXX_INCLUDE_DIR)
      add_library(pqxx::pqxx SHARED IMPORTED)
      set_target_properties(pqxx::pqxx PROPERTIES
        IMPORTED_LOCATION "${PQXX_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PQXX_INCLUDE_DIR}")
      set(pqxx_FOUND TRUE)
    endif()
  elseif(UNIX)
    # Linux with Homebrew (linuxbrew)
    set(HOMEBREW_PREFIX "/home/linuxbrew/.linuxbrew")
    find_library(PQXX_LIBRARY pqxx
      HINTS "${HOMEBREW_PREFIX}/opt/libpqxx/lib"
            "${HOMEBREW_PREFIX}/lib")
    find_library(PQ_LIBRARY pq
      HINTS "${HOMEBREW_PREFIX}/opt/libpq/lib"
            "${HOMEBREW_PREFIX}/lib")
    find_path(PQXX_INCLUDE_DIR pqxx/pqxx
      HINTS "${HOMEBREW_PREFIX}/opt/libpqxx/include"
            "${HOMEBREW_PREFIX}/include")

    if(PQXX_LIBRARY AND PQ_LIBRARY AND PQXX_INCLUDE_DIR)
      message(STATUS "Found libpqxx: ${PQXX_LIBRARY}")
      message(STATUS "Found libpq: ${PQ_LIBRARY}")
      message(STATUS "Found pqxx includes: ${PQXX_INCLUDE_DIR}")
      add_library(pqxx::pqxx SHARED IMPORTED)
      set_target_properties(pqxx::pqxx PROPERTIES
        IMPORTED_LOCATION "${PQXX_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PQXX_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${PQ_LIBRARY}")
      set(pqxx_FOUND TRUE)
    endif()
  endif()
endif()

add_executable(${PROJECT_NAME}
  main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE include)

if(TARGET pqxx::pqxx)
  target_link_libraries(${PROJECT_NAME} PRIVATE pqxx::pqxx OpenSSL::SSL OpenSSL::Crypto)
elseif(pqxx_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE pqxx OpenSSL::SSL OpenSSL::Crypto)
else()
  message(FATAL_ERROR "libpqxx not found: install via Homebrew (brew install libpqxx) or set pqxx_DIR")
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION bin
)