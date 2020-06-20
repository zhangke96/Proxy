// Copyright [2020] zhangke

#include "server/proxy_server.h"

#include <muduo/base/Logging.h>

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

void ProxyServer::OnConnection(const muduo::net::TcpConnectionPtr &conn) {
  if (proxy_instances_.find(conn.get()) == proxy_instances_.end()) {
    LOG_INFO << "new connection from:" << conn->peerAddress().toIpPort();
    std::shared_ptr<ProxyInstance> proxy_instance =
        std::make_shared<ProxyInstance>(conn->getLoop(), conn);
    proxy_instances_[conn.get()] = proxy_instance;
    proxy_instance->Init();
  } else {
    LOG_INFO << "connection close:" << conn->peerAddress().toIpPort();
    proxy_instances_[conn.get()]->Stop(
        std::bind(&ProxyServer::OnProxyInstanceStop, this, conn.get()));
  }
}

void ProxyServer::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                            muduo::net::Buffer *buf, muduo::Timestamp time) {
  auto index = proxy_instances_.find(conn.get());
  assert(index != proxy_instances_.end());
  (index->second)->OnMessage(conn, buf, time);
}

void ProxyServer::OnProxyInstanceStop(muduo::net::TcpConnection *conn) {
  LOG_INFO << "proxy server stop finish " << conn->peerAddress().toIpPort();
  proxy_instances_.erase(conn);
}
