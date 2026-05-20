add_executable(crypto_test CryptoTest.cpp)

target_link_libraries(crypto_test PRIVATE zapshare_shared)

add_test(NAME crypto_test COMMAND crypto_test)
