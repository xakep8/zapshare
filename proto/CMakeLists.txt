# Protobuf support: generate C++ sources for any .proto files in /proto
file(GLOB ZAPSHARE_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.proto)
if(ZAPSHARE_BUILD_PROTO AND ZAPSHARE_PROTO_FILES)
    find_package(Protobuf QUIET)
    if(NOT Protobuf_FOUND)
        find_package(Protobuf REQUIRED)
    endif()

    protobuf_generate_cpp(
        ZAPSHARE_PROTO_SRCS
        ZAPSHARE_PROTO_HDRS
        ${ZAPSHARE_PROTO_FILES}
    )

    add_library(
        zapshare_proto
        STATIC
        ${ZAPSHARE_PROTO_SRCS}
        ${ZAPSHARE_PROTO_HDRS}
    )
    target_include_directories(
        zapshare_proto
        PUBLIC ${CMAKE_CURRENT_BINARY_DIR}
    )
    if(TARGET Protobuf::libprotobuf)
        target_link_libraries(zapshare_proto PUBLIC Protobuf::libprotobuf)
    else()
        target_link_libraries(zapshare_proto PUBLIC ${Protobuf_LIBRARIES})
    endif()
endif()