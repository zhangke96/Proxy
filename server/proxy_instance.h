// Copyright [2020] zhangke
#ifndef SERVER_PROXY_INSTANCE_H_
#define SERVER_PROXY_INSTANCE_H_

#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Acceptor.h"
#include "TcpServer.h"
#include "common/message_dispatch.h"

struct Connection {
  explicit Connection(muduo::net::TcpConnectionPtr conn)
      : conn(conn), proxy_accept(false), server_block(false) {}
  Connection() = default;
  Connection(const Connection &) = default;
  muduo::net::TcpConnectionPtr conn;
  bool proxy_accept;
  bool server_block;  // 标识真实server对应的连接是否block
  std::vector<std::string> pending_message;
};

typedef std::function<void()> StopCb;

class ProxyInstance : public std::enable_shared_from_this<ProxyInstance> {
 public:
  ProxyInstance(muduo::net::EventLoop *loop,
                const muduo::net::TcpConnectionPtr &conn);
  ~ProxyInstance();
  void Init();
  void Stop(StopCb cb);
  void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                 muduo::net::Buffer *buf, muduo::Timestamp time) {
    dispatcher_->OnMessage(conn, buf, time);
  }
  void OnNewConnection(int sockfd, const muduo::net::InetAddress &);
  void OnClientConnection(const muduo::net::TcpConnectionPtr &);
  void OnClientMessage(const muduo::net::TcpConnectionPtr &,
                       muduo::net::Buffer *, muduo::Timestamp);
  void OnClientClose(const muduo::net::TcpConnectionPtr &);
  void EntryAddConnection(MessagePtr message,
                          const muduo::net::TcpConnectionPtr &);
  void AddConnectionTimeout(const muduo::net::TcpConnectionPtr &);
  void EntryData(const muduo::net::TcpConnectionPtr &conn, ProxyMessagePtr,
                 const muduo::net::TcpConnectionPtr &client_conn);
  void EntryCloseConnection(MessagePtr, uint64_t conn_id);
  void EntryPauseSend(MessagePtr, uint64_t conn_id);
  void EntryResumeSend(MessagePtr, uint64_t conn_id);
  void EntryHeartBeat(MessagePtr);
  uint64_t GetConnId();
  uint32_t GetSourceEntity();
  std::shared_ptr<ProxyInstance> this_ptr() { return shared_from_this(); }

 private:
  void HandleListenRequest(const muduo::net::TcpConnectionPtr,
                           ProxyMessagePtr request_head, MessagePtr message);
  void HandleDataRequest(const muduo::net::TcpConnectionPtr,
                         ProxyMessagePtr message);
  void HandleCloseConnRequest(const muduo::net::TcpConnectionPtr,
                              ProxyMessagePtr request_head, MessagePtr message);
  void HandlePauseSendRequest(const muduo::net::TcpConnectionPtr,
                              ProxyMessagePtr request_head, MessagePtr message);
  void HandleResumeSendRequest(const muduo::net::TcpConnectionPtr,
                               ProxyMessagePtr request_head,
                               MessagePtr message);
  void HandleHeartbeat(const muduo::net::TcpConnectionPtr,
                       ProxyMessagePtr request_head, MessagePtr message);
  std::pair<bool, std::string> StartListen();
  void RemoveConnecion(uint64_t conn_id);
  void StopClientRead(uint64_t conn_id = 0, bool server_block = false);
  void ResumeClientRead(uint64_t conn_id = 0, bool sever_block = false);
  void OnHighWaterMark(bool is_proxy_conn, const muduo::net::TcpConnectionPtr &,
                       size_t);
  void OnWriteComplete(bool is_proxy_conn,
                       const muduo::net::TcpConnectionPtr &);
  void CheckListen();
  void CheckStop();
  void SendHeartBeat();
  muduo::net::EventLoop *loop_;
  std::unique_ptr<MessageDispatch> dispatcher_;
  muduo::net::TcpConnectionPtr proxy_conn_;
  muduo::net::TimerId check_listen_timer_;
  // std::unique_ptr<TcpServer> server_;

  MessagePtr listen_response_msg_;
  uint64_t conn_id_;
  uint32_t source_entity_;
  muduo::net::InetAddress listen_addr_;
  std::map<uint64_t, Connection> conn_map_;
  // std::map<uint64_t, muduo::net::TcpConnectionPtr> conn_map_;
  // std::map<uint64_t, std::vector<std::string>> pending_message_;
  std::unique_ptr<Acceptor> acceptor_;
  std::shared_ptr<muduo::net::EventLoopThreadPool> thread_pool_;
  bool proxy_client_connect_;
  muduo::net::TimerId heartbeat_timer_;
  StopCb stop_cb_;
};

#endif  // SERVER_PROXY_INSTANCE_H_
