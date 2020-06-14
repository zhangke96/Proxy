// Copyright [2020] zhangke
#ifndef CLIENT_TCP_CLIENT_H_
#define CLIENT_TCP_CLIENT_H_

#include <muduo/base/Mutex.h>
#include <muduo/net/Connector.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>

#include <memory>

class TcpClient {
 public:
  TcpClient(muduo::net::EventLoop *loop, const muduo::net::InetAddress &addr);
  void Connect();
  void Stop();
  void Disconnect();
  void SetConnectionCallback(muduo::net::ConnectionCallback cb) {
    connection_callback_ = cb;
  }
  void SetMessageCallback(muduo::net::MessageCallback cb) {
    msg_callback_ = cb;
  }
  void SetCloseCallback(muduo::net::CloseCallback cb) { close_callback_ = cb; }
  void OnNewConnection(int fd);
  void DestroyConn();
  muduo::net::TcpConnectionPtr Connection();

 private:
  muduo::net::EventLoop *loop_;
  muduo::net::InetAddress server_addr_;
  muduo::net::ConnectionCallback connection_callback_;
  muduo::net::MessageCallback msg_callback_;
  muduo::net::CloseCallback close_callback_;
  std::shared_ptr<muduo::net::Connector> connector_;
  muduo::net::TcpConnectionPtr connection_;
  bool connect_;
  muduo::MutexLock mutex_;
};

#endif  // CLIENT_TCP_CLIENT_H_
