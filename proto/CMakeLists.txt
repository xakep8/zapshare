# Protobuf support: generate C++ sources for any .proto files in /proto
file(GLOB_RECURSE ZAPSHARE_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.proto)
if(ZAPSHARE_BUILD_PROTO AND ZAPSHARE_PROTO_FILES)
    find_package(protobuf CONFIG REQUIRED)

    protobuf_generate(
        LANGUAGE cpp
        OUT_VAR ZAPSHARE_PROTO_SRCS
        PROTOS ${ZAPSHARE_PROTO_FILES}
    )

    set(ZAPSHARE_PROTO_HDRS)
    foreach(proto_src ${ZAPSHARE_PROTO_SRCS})
        string(REPLACE ".pb.cc" ".pb.h" proto_hdr "${proto_src}")
        list(APPEND ZAPSHARE_PROTO_HDRS "${proto_hdr}")
    endforeach()

    add_library(
        zapshare_proto
        STATIC
        ${ZAPSHARE_PROTO_SRCS}
        ${ZAPSHARE_PROTO_HDRS}
    )
    target_link_libraries(
        zapshare_proto
        PUBLIC protobuf::libprotobuf
    )
    target_include_directories(
        zapshare_proto
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
            $<INSTALL_INTERFACE:include>
    )

    install(
        TARGETS zapshare_proto
        EXPORT zapshareTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        INCLUDES DESTINATION include
    )

    install(FILES ${ZAPSHARE_PROTO_HDRS} DESTINATION include)
endif()
