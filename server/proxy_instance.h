// Copyright [2020] zhangke
#ifndef SERVER_PROXY_INSTANCE_H_
#define SERVER_PROXY_INSTANCE_H_

#include <muduo/net/Acceptor.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/pb_dispatch.h"
#include "TcpServer.h"

struct Connection {
  explicit Connection(muduo::net::TcpConnectionPtr conn)
      : conn(conn),
        server_close(false),
        client_close(false),
        proxy_accept(false) {}
  Connection() = default;
  Connection(const Connection &) = default;
  muduo::net::TcpConnectionPtr conn;
  bool server_close;
  bool client_close;
  bool proxy_accept;
  std::vector<std::string> pending_message;
};

class ProxyInstance : public std::enable_shared_from_this<ProxyInstance>,
                      public PbDispatch {
 public:
  ProxyInstance(muduo::net::EventLoop *loop,
                const muduo::net::TcpConnectionPtr &conn);
  void Init();
  void OnNewConnection(int sockfd, const muduo::net::InetAddress &);
  void OnClientConnection(const muduo::net::TcpConnectionPtr &);
  void OnClientMessage(const muduo::net::TcpConnectionPtr &,
                       muduo::net::Buffer *, muduo::Timestamp);
  void OnClientClose(const muduo::net::TcpConnectionPtr &);
  void EntryAddConnection(MessagePtr message,
                          const muduo::net::TcpConnectionPtr &);
  void EntryData(MessagePtr, const muduo::net::TcpConnectionPtr &);
  void EntryCloseConnection(MessagePtr, uint64_t conn_id);
  uint64_t GetConnId();
  uint32_t GetSourceEntity();
  std::shared_ptr<ProxyInstance> this_ptr() { return shared_from_this(); }

 private:
  void HandleListenRequest(const muduo::net::TcpConnectionPtr,
                           MessagePtr message);
  void HandleDataRequest(const muduo::net::TcpConnectionPtr,
                         MessagePtr message);
  void HandleCloseConnRequest(const muduo::net::TcpConnectionPtr,
                              MessagePtr message);
  void StartListen();
  void RemoveConnecion(uint64_t conn_id);
  muduo::net::EventLoop *loop_;
  muduo::net::TcpConnectionPtr proxy_conn_;
  // std::unique_ptr<TcpServer> server_;

  MessagePtr listen_response_msg_;
  uint64_t conn_id_;
  uint32_t source_entity_;
  muduo::net::InetAddress listen_addr_;
  std::map<uint64_t, Connection> conn_map_;
  // std::map<uint64_t, muduo::net::TcpConnectionPtr> conn_map_;
  // std::map<uint64_t, std::vector<std::string>> pending_message_;
  std::unique_ptr<muduo::net::Acceptor> acceptor_;
  std::shared_ptr<muduo::net::EventLoopThreadPool> thread_pool_;
};

#endif  // SERVER_PROXY_INSTANCE_H_
