set(COMMON_SRC
    message_util.cc
    message.pb.cc
    pb_dispatch.cc
)
add_library(common STATIC ${COMMON_SRC})
target_link_libraries(common protobuf)
