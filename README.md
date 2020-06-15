# 超级代理程序
1. ### 实现功能
    转发内网服务端程序的流量到公网，从而公网可以访问到内网的资源。
2. ### 实现思路
    内网程序首先像公网转发程序注册，公网程序bind指定的端口，之后所有的连接，收到数据都会转发到内网程序的连接。  
    为了兼容现有的服务端程序，不修改代码，不重新编译，因此需要hook系统调用，比如listen，close
3. ### 如何使用
    * #### 编译
       依赖muduo，因为这里需要编译出动态库，需要修改muduo的cmake，添加编译选项-fPIC。  
       `./do_cmake.sh && cd build && make`
    * #### server
       `./proxy_server`，如果需要修改监听ip和端口需要修改文件`server.cc`
    * #### client
       执行需要代理的程序之前需要设置几个环境变量
        ```sh
        LD_PRELOAD=~/code/proxy/build/client/libproxy_client.so  // 动态库路径
        PROXY_SERVER_ADDR=127.0.0.1                              // Proxy Server的ip地址
        PROXY_SERVER_PORT=62580                                  // Proxy Server的监听端口
        PROXY_LISTEN_PORT=8010                                   // 代理到Proxy Server所在机器的监听端口
        ````
4. ### 局限
    因为client需要使用动态库hook系统调用，所以多进程的程序可能无法正常工作，如果在调用了listen之后再调用fork，client端的处理代码都没了。
