// Copyright [2020] zhangke

#include <muduo/base/Logging.h>
#include <muduo/net/InetAddress.h>
#include <unistd.h>

#include <iostream>
#include <memory>

#include "client/proxy_client.h"

void PrintUsage(const char *command) {
  std::cout << "Usage:" << command << " -s server_address"
            << " -p server_port"
            << " -t transfer_port"
            << " -S proxy_server_address"
            << " -P proxy_server_port"
            << " -L log_level[trace/debug/info/warn]"
            << " -h help" << std::endl;
}
int main(int argc, char *argv[]) {
  // server address, server port, transfer port, proxy server address, proxy
  // server port
  const char *server_address_p = nullptr, *proxy_server_address_p = nullptr;
  muduo::Logger::LogLevel log_level = muduo::Logger::DEBUG;
  const char *level_str = nullptr;
  uint16_t server_port = 0, transfer_port = 0, proxy_server_port = 0;
  int ch;
  int port = 0;
  while ((ch = getopt(argc, argv, "s:p:t:S:P:L:h")) != -1) {
    switch (ch) {
      case 's':
        server_address_p = optarg;
        std::cout << "server_address:" << server_address_p << std::endl;
        break;
      case 'p':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid server port:" << port << std::endl;
          exit(1);
        } else {
          server_port = static_cast<uint16_t>(port);
          std::cout << "server_port:" << server_port << std::endl;
        }
        break;
      case 't':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid tranfer port:" << port << std::endl;
          exit(1);
        } else {
          transfer_port = static_cast<uint16_t>(port);
          std::cout << "transfer_port:" << transfer_port << std::endl;
        }
        break;
      case 'S':
        proxy_server_address_p = optarg;
        std::cout << "proxy_server_address:" << proxy_server_address_p
                  << std::endl;
        break;
      case 'P':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid proxy server port:" << port << std::endl;
          exit(1);
        } else {
          proxy_server_port = static_cast<uint16_t>(port);
          std::cout << "proxy_server_port:" << proxy_server_port << std::endl;
        }
        break;
      case 'L':
        level_str = optarg;
        if (strcmp(level_str, "trace") == 0) {
          log_level = muduo::Logger::TRACE;
        } else if (strcmp(level_str, "debug") == 0) {
          log_level = muduo::Logger::DEBUG;
        } else if (strcmp(level_str, "info") == 0) {
          log_level = muduo::Logger::INFO;
        } else if (strcmp(level_str, "warn") == 0) {
          log_level = muduo::Logger::WARN;
        }
        std::cout << "log level:" << log_level << std::endl;
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
  muduo::Logger::setLogLevel(log_level);
  muduo::net::InetAddress server_address(server_port);
  bool ret =
      muduo::net::InetAddress::resolve(server_address_p, &server_address);
  if (!ret) {
    LOG_FATAL << "resolve:" << server_address_p << " failed";
  }
  muduo::net::InetAddress proxy_server_address(proxy_server_port);
  ret = muduo::net::InetAddress::resolve(proxy_server_address_p,
                                         &proxy_server_address);
  if (!ret) {
    LOG_FATAL << "resolve:" << proxy_server_address_p << " failed";
  }
  muduo::net::EventLoopThread event_loop_thread;
  muduo::net::EventLoop *loop = event_loop_thread.startLoop();
  std::shared_ptr<ProxyClient> proxy_client(std::make_shared<ProxyClient>(
      loop, proxy_server_address, server_address, transfer_port));
  proxy_client->Start();
  muduo::net::EventLoop main_loop;
  main_loop.loop();
}