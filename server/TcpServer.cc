// Copyright [2020] zhangke

#include "server/TcpServer.h"

#include <muduo/base/Logging.h>
#include <muduo/net/SocketsOps.h>

#include <stdio.h>  // snprintf

TcpServer::TcpServer(muduo::net::EventLoop* loop,
                     const muduo::net::InetAddress& listenAddr,
                     const std::string& nameArg, Option option)
    : loop_(CHECK_NOTNULL(loop)),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(
          new muduo::net::Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(new muduo::net::EventLoopThreadPool(loop, name_)),
      connectionCallback_(muduo::net::defaultConnectionCallback),
      messageCallback_(muduo::net::defaultMessageCallback),
      nextConnId_(1) {
  acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
                                                std::placeholders::_1,
                                                std::placeholders::_2));
}

TcpServer::~TcpServer() {
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (auto& item : connections_) {
    muduo::net::TcpConnectionPtr conn(item.second.conn);
    item.second.conn.reset();
    conn->getLoop()->runInLoop(
        std::bind(&muduo::net::TcpConnection::connectDestroyed, conn));
  }
  // for (std::set<Connection>::iterator index = connections_.begin();
  //      index != connections_.end(); ++index) {
  //   muduo::net::TcpConnectionPtr conn(index->conn);
  //   (index->conn) = nullptr;
  //   conn->getLoop()->runInLoop(
  //       std::bind(&muduo::net::TcpConnection::connectDestroyed, conn));
  // }
}

void TcpServer::setThreadNum(int numThreads) {
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}

void TcpServer::start() {
  if (started_.getAndSet(1) == 0) {
    threadPool_->start(threadInitCallback_);

    assert(!acceptor_->listenning());
    loop_->runInLoop(std::bind(&muduo::net::Acceptor::listen,
                               muduo::get_pointer(acceptor_)));
  }
}

void TcpServer::shutdownConn(const muduo::net::TcpConnectionPtr& conn) {
  loop_->runInLoop([=]() {
    auto& connection = connections_[conn.get()];
    // client已经关闭
    if (connection.client_close) {
      if (!connection.server_close) {
        connection.server_close = true;
        removeConnection(conn);
      }
    } else {
      // client还没关闭
      if (!connection.server_close) {
        connection.server_close = true;
        conn->shutdown();
      }
    }
  });
}

void TcpServer::onClientClose(const muduo::net::TcpConnectionPtr& conn) {
  loop_->runInLoop([=]() {
    auto& connection = connections_[conn.get()];
    // server已经关闭
    if (connection.server_close) {
      assert(!connection.client_close);
      connection.client_close = true;
      removeConnection(conn);
    } else {
      // server还没关闭
      connection.client_close = true;
      if (closeCallback_) {
        closeCallback_(conn);
      }
    }
  });
}

void TcpServer::newConnection(int sockfd,
                              const muduo::net::InetAddress& peerAddr) {
  loop_->assertInLoopThread();
  muduo::net::EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
  ++nextConnId_;
  std::string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_ << "] - new connection ["
           << connName << "] from " << peerAddr.toIpPort();
  muduo::net::InetAddress localAddr(muduo::net::sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  muduo::net::TcpConnectionPtr conn(new muduo::net::TcpConnection(
      ioLoop, connName, sockfd, localAddr, peerAddr));
  connections_.insert(std::make_pair(conn.get(), Connection(conn)));
  // connections_[conn.get()] = std::move(Connection(conn));
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this,
                                   std::placeholders::_1));  // FIXME: unsafe
  ioLoop->runInLoop(
      std::bind(&muduo::net::TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const muduo::net::TcpConnectionPtr& conn) {
  // FIXME: unsafe
  loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(
    const muduo::net::TcpConnectionPtr& conn) {
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  size_t n = connections_.erase(conn.get());
  (void)n;
  assert(n == 1);
  muduo::net::EventLoop* ioLoop = conn->getLoop();
  ioLoop->queueInLoop(
      std::bind(&muduo::net::TcpConnection::connectDestroyed, conn));
}
