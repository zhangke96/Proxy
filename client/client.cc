// Copyright [2020] zhangke

#include <muduo/net/InetAddress.h>
#include <unistd.h>

#include <iostream>
#include <memory>

#include "client/proxy_client.h"

void PrintUsage(const char *command) {
  std::cout << "Usage:" << command << " -s server_ip"
            << " -p server_port"
            << " -t transfer_port"
            << " -S proxy_server_ip"
            << " -P proxy_server_port"
            << " -h help" << std::endl;
}
int main(int argc, char *argv[]) {
  // server address, server port, transfer port, proxy server address, proxy
  // server port
  const char *server_address_p = nullptr, *proxy_server_address_p = nullptr;
  uint16_t server_port = 0, transfer_port = 0, proxy_server_port = 0;
  int ch;
  int port = 0;
  while ((ch = getopt(argc, argv, "s:p:t:S:P:h")) != -1) {
    switch (ch) {
      case 's':
        server_address_p = optarg;
        std::cout << "server_address:" << *server_address_p;
        break;
      case 'p':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid server port:" << port;
          exit(1);
        } else {
          server_port = static_cast<uint16_t>(port);
          std::cout << "server_port:" << server_port;
        }
        break;
      case 't':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid tranfer port:" << port;
          exit(1);
        } else {
          transfer_port = static_cast<uint16_t>(port);
          std::cout << "transfer_port:" << transfer_port;
        }
        break;
      case 'S':
        proxy_server_address_p = optarg;
        std::cout << "proxy_server_address:" << *proxy_server_address_p;
        break;
      case 'P':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid proxy server port:" << port;
          exit(1);
        } else {
          proxy_server_port = static_cast<uint16_t>(port);
          std::cout << "proxy_server_port:" << proxy_server_port;
        }
        break;
      case 'h':
        PrintUsage(argv[0]);
        exit(0);
        break;
      default:
        PrintUsage(argv[0]);
        exit(0);
        break;
    }
  }
  if (server_address_p == nullptr || proxy_server_address_p == nullptr ||
      server_port == 0 || transfer_port == 0 || proxy_server_port == 0) {
    std::cout << "Missing parameter" << std::endl;
    PrintUsage(argv[0]);
    exit(1);
  }
  muduo::net::InetAddress server_address(server_address_p, server_port);
  muduo::net::InetAddress proxy_server_address(proxy_server_address_p,
                                               proxy_server_port);
  muduo::net::EventLoopThread event_loop_thread;
  muduo::net::EventLoop *loop = event_loop_thread.startLoop();
  std::shared_ptr<ProxyClient> proxy_client(std::make_shared<ProxyClient>(
      loop, proxy_server_address, server_address, transfer_port));
  proxy_client->Start();
  muduo::net::EventLoop main_loop;
  main_loop.loop();
}