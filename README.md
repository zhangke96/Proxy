# 超级代理程序
1. ### 实现功能
    转发内网服务端程序的流量到公网，从而公网可以访问到内网的资源。
2. ### 实现思路
    内网程序首先像公网转发程序注册，公网程序bind指定的端口，之后所有的连接，收到数据都会转发到内网程序的连接。  
    为了兼容现有的服务端程序，不修改代码，client实现代理功能。  
3. ### 如何使用
    * #### 编译
       依赖[muduo](https://github.com/chenshuo/muduo)  
       `./do_cmake.sh && cd build && make`
    * #### server
       `./proxy_server`，如果需要修改监听ip和端口需要修改文件`server.cc`
    * #### client
       `./proxy_client -s server_ip -p server_port -t transform_port -S proxy_server_ip -P proxy_server_port`
