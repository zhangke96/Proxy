// Copyright [2021] zhangke

#include "server/Acceptor.h"

#include <errno.h>
#include <fcntl.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include "muduo/net/InetAddress.h"
#include "muduo/net/SocketsOps.h"
//#include <sys/types.h>
//#include <sys/stat.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr,
                   bool reuseport)
    : loop_(loop),
      acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())),
      acceptChannel_(loop, acceptSocket_.fd()),
      listenAddr_(listenAddr),
      listening_(false) {
  acceptSocket_.setReuseAddr(true);
  acceptSocket_.setReusePort(reuseport);
  acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor() {
  acceptChannel_.disableAll();
  acceptChannel_.remove();
}

std::pair<bool, std::string> Acceptor::listen() {
  loop_->assertInLoopThread();
  listening_ = true;
  auto bind_result = acceptSocket_.tryBindAddress(listenAddr_);
  if (!bind_result.first) {
    LOG_ERROR << "bind addr: " << listenAddr_.toIpPort() << " failed";
    return {false, std::string(strerror_tl(bind_result.second))};
  }
  auto listen_result = acceptSocket_.tryListen();
  if (!listen_result.first) {
    LOG_ERROR << "listen addr: " << listenAddr_.toIpPort() << " failed";
    return {false, std::string(strerror_tl(listen_result.second))};
  }
  acceptChannel_.enableReading();
  return {true, "success"};
}

void Acceptor::handleRead() {
  loop_->assertInLoopThread();
  InetAddress peerAddr;
  // FIXME loop until no more
  int connfd = acceptSocket_.accept(&peerAddr);
  if (connfd >= 0) {
    // string hostport = peerAddr.toIpPort();
    // LOG_TRACE << "Accepts of " << hostport;
    if (newConnectionCallback_) {
      newConnectionCallback_(connfd, peerAddr);
    } else {
      sockets::close(connfd);
    }
  } else {
    LOG_SYSERR << "in Acceptor::handleRead";
  }
}
