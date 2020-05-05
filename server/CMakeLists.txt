set(proxy_server_srcs
    proxy_instance.cc
    proxy_server.cc
    server.cc
)

add_executable(proxy_server ${proxy_server_srcs})
target_link_libraries(proxy_server common ${muduo_deps})
set_target_properties(proxy_server PROPERTIES VERSION ${PROXY_VERSION})
