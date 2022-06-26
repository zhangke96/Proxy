// Copyright [2021] zhangke

#ifndef SERVER_ACCEPTOR_H_
#define SERVER_ACCEPTOR_H_

#include <muduo/base/noncopyable.h>
#include <muduo/net/Channel.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/Socket.h>

#include <functional>
#include <string>
#include <utility>

class muduo::net::EventLoop;
class muduo::net::InetAddress;

///
/// Acceptor of incoming TCP connections.
///
class Acceptor : muduo::noncopyable {
 public:
  typedef std::function<void(int sockfd, const muduo::net::InetAddress&)>
      NewConnectionCallback;

  Acceptor(muduo::net::EventLoop* loop,
           const muduo::net::InetAddress& listenAddr, bool reuseport);
  ~Acceptor();

  void setNewConnectionCallback(const NewConnectionCallback& cb) {
    newConnectionCallback_ = cb;
  }

  std::pair<bool, std::string> listen();

  bool listening() const { return listening_; }

  // Deprecated, use the correct spelling one above.
  // Leave the wrong spelling here in case one needs to grep it for error
  // messages. bool listenning() const { return listening(); }

 private:
  void handleRead();

  muduo::net::EventLoop* loop_;
  muduo::net::Socket acceptSocket_;
  muduo::net::Channel acceptChannel_;
  NewConnectionCallback newConnectionCallback_;
  muduo::net::InetAddress listenAddr_;
  bool listening_;
};

#endif  // MUDUO_NET_ACCEPTOR_H
