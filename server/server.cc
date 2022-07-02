// Copyright [2020] zhangke
#include <muduo/base/Logging.h>
#include <muduo/base/LogFile.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include "server/proxy_server.h"

void PrintUsage(const char *command) {
  std::cout << "Usage:"
            << " -s listen_address"
            << " -p listen_port"
            << " -l log_level[trace/debug/info/warn]"
            << " -h help" << std::endl;
}

std::unique_ptr<muduo::LogFile> g_logFile;

void outputFunc(const char* msg, int len) {
  g_logFile->append(msg, len);
}

void flushFunc() {
  g_logFile->flush();
}


int main(int argc, char *argv[]) {
  const char *listen_address_p = nullptr, *level_str = nullptr;
  muduo::Logger::LogLevel log_level = muduo::Logger::DEBUG;
  uint16_t listen_port = 0;
  int port = 0, ch = 0;
  while ((ch = getopt(argc, argv, "s:p:l:h")) != -1) {
    switch (ch) {
      case 's':
        listen_address_p = optarg;
        std::cout << "listen_address:" << listen_address_p << std::endl;
        break;
      case 'p':
        port = atoi(optarg);
        if (port <= 0 || port >= 65535) {
          std::cout << "invalid listen port:" << port << std::endl;
          exit(1);
        } else {
          listen_port = static_cast<uint16_t>(port);
          std::cout << "listen_port:" << listen_port << std::endl;
        }
        break;
      case 'l':
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
  if (listen_address_p == nullptr || listen_port == 0) {
    std::cout << "Missing parameter" << std::endl;
    PrintUsage(argv[0]);
    exit(1);
  }
  muduo::net::EventLoop loop;
  muduo::net::InetAddress address(listen_address_p, listen_port);
   char name[256] = {0};
  strncpy(name, argv[0], sizeof(name) - 1);
  g_logFile.reset(new muduo::LogFile(::basename(name), 10*1024*1024, true, 1, 1));
  muduo::Logger::setLogLevel(log_level);
  muduo::Logger::setOutput(outputFunc);
  muduo::Logger::setFlush(flushFunc);
  ProxyServer proxy_server(&loop, address);
  proxy_server.Start();
  loop.loop();
  return 0;
}
