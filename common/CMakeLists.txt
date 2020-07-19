set(COMMON_SRC
    message_dispatch.cc
    message_util.cc
    message.pb.cc
    proto.cc
)
add_library(common STATIC ${COMMON_SRC})
target_link_libraries(common libprotobuf.a)
