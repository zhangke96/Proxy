// Copyright [2020] zhangke
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include "server/proxy_server.h"

int main(int argc, char *argv[]) {
  muduo::net::EventLoop loop;
  muduo::net::InetAddress address("127.0.0.1", 62580);
  ProxyServer proxy_server(&loop, address);
  proxy_server.Start();
  loop.loop();
  return 0;
}
