// Copyright [2020] zhangke

#include "client/tcp_client.h"
#include <muduo/net/SocketsOps.h>

TcpClient::TcpClient(muduo::net::EventLoop* loop,
                     const muduo::net::InetAddress& addr)
    : loop_(loop),
      server_addr_(addr),
      connector_(std::make_shared<muduo::net::Connector>(loop, addr)),
      connect_(false) {
  connector_->setNewConnectionCallback(
      std::bind(&TcpClient::OnNewConnection, this, std::placeholders::_1));
}

void TcpClient::Connect() {
  connect_ = true;
  connector_->start();
}

void TcpClient::Stop() {
  connect_ = false;
  connector_->stop();
}

void TcpClient::Disconnect() {
  // shutdown write
  connect_ = false;
  connection_->shutdown();
}

void TcpClient::OnNewConnection(int sock_fd) {
  loop_->assertInLoopThread();
  muduo::net::InetAddress local_addr(
      muduo::net::sockets::getLocalAddr(sock_fd));
  muduo::net::TcpConnectionPtr conn(std::make_shared<muduo::net::TcpConnection>(
      loop_, "", sock_fd, local_addr, server_addr_));
  conn->setConnectionCallback(connection_callback_);
  conn->setMessageCallback(msg_callback_);
  conn->setCloseCallback(close_callback_);
  {
    muduo::MutexLockGuard lock(mutex_);
    connection_ = conn;
  }
  conn->connectEstablished();
}

void TcpClient::DestroyConn() {
  loop_->assertInLoopThread();
  loop_->queueInLoop(
      std::bind(&muduo::net::TcpConnection::connectDestroyed, connection_));
  {
    muduo::MutexLockGuard lock(mutex_);
    connection_.reset();
  }
}

muduo::net::TcpConnectionPtr TcpClient::Connection() {
  muduo::MutexLockGuard lock(mutex_);
  return connection_;
}
