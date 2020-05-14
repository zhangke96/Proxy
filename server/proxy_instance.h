// Copyright [2020] zhangke
#ifndef SERVER_PROXY_INSTANCE_H_
#define SERVER_PROXY_INSTANCE_H_

#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/pb_dispatch.h"

class ProxyInstance : public std::enable_shared_from_this<ProxyInstance>,
                      public PbDispatch {
 public:
  ProxyInstance(muduo::net::EventLoop *loop,
                const muduo::net::TcpConnectionPtr &conn);
  void Init();
  void OnClientConnection(const muduo::net::TcpConnectionPtr &);
  void OnClientMessage(const muduo::net::TcpConnectionPtr &,
                       muduo::net::Buffer *, muduo::Timestamp);
  void EntryAddConnection(MessagePtr message,
                          const muduo::net::TcpConnectionPtr &);
  void EntryData(MessagePtr, const muduo::net::TcpConnectionPtr &);
  uint64_t GetConnId();
  uint32_t GetSourceEntity();
  std::shared_ptr<ProxyInstance> this_ptr() { return shared_from_this(); }

 private:
  void HandleListenRequest(const muduo::net::TcpConnectionPtr,
                           MessagePtr message);
  void HandleDataRequest(const muduo::net::TcpConnectionPtr,
                         MessagePtr message);
  muduo::net::EventLoop *loop_;
  muduo::net::TcpConnectionPtr proxy_conn_;
  std::unique_ptr<muduo::net::TcpServer> server_;

  MessagePtr listen_response_msg_;
  uint64_t conn_id_;
  uint32_t source_entity_;
  std::map<uint64_t, muduo::net::TcpConnectionPtr> conn_map_;
  std::map<uint64_t, std::vector<std::string>> pending_message_;
};

#endif  // SERVER_PROXY_INSTANCE_H_
