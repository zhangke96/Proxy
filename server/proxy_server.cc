// Copyright [2020] zhangke

#include "server/proxy_server.h"

ProxyServer::ProxyServer(muduo::net::EventLoop *loop,
                         const muduo::net::InetAddress &listen_address)
    : loop_(loop), listen_address_(listen_address) {}

void ProxyServer::Start() {
  server_.reset(new muduo::net::TcpServer(loop_, listen_address_, "server"));
  server_->setConnectionCallback(
      std::bind(&ProxyServer::OnConnection, this, std::placeholders::_1));
  server_->setMessageCallback(
      std::bind(&ProxyServer::OnMessage, this, std::placeholders::_1,
                std::placeholders::_2, std::placeholders::_3));
  server_->start();
}

void ProxyServer::OnConnection(const muduo::net::TcpConnectionPtr &new_conn) {
  std::shared_ptr<ProxyInstance> proxy_instance =
      std::make_shared<ProxyInstance>(new_conn->getLoop(), new_conn);
  proxy_instances_[new_conn.get()] = proxy_instance;
  proxy_instance->Init();
}

void ProxyServer::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                            muduo::net::Buffer *buf, muduo::Timestamp time) {
  auto index = proxy_instances_.find(conn.get());
  assert(index != proxy_instances_.end());
  (index->second)->OnMessage(conn, buf, time);
}
