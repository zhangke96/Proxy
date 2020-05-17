set(proxy_client_srcs
    hook.cc
    proxy_client.cc
    tcp_client.cc
)

add_library(proxy_client SHARED ${proxy_client_srcs})
target_link_libraries(proxy_client common dl ${muduo_deps})
set_target_properties(proxy_client PROPERTIES VERSION ${PROXY_VERSION})
