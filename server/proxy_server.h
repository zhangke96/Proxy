// Copyright [2020] zhangke
#ifndef SERVER_PROXY_SERVER_H_
#define SERVER_PROXY_SERVER_H_

#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#include <map>
#include <memory>

#include "server/proxy_instance.h"

class ProxyServer {
 public:
  explicit ProxyServer(muduo::net::EventLoop *loop,
                       const muduo::net::InetAddress &listen_address);
  void Start();
  void OnConnection(const muduo::net::TcpConnectionPtr &conn);
  void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                 muduo::net::Buffer *buf, muduo::Timestamp);

 private:
  muduo::net::EventLoop *loop_;
  muduo::net::InetAddress listen_address_;
  std::unique_ptr<muduo::net::TcpServer> server_;
  std::map<muduo::net::TcpConnection *, std::shared_ptr<ProxyInstance>>
      proxy_instances_;
};

#endif  // SERVER_PROXY_SERVER_H_
