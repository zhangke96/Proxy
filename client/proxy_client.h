// Copyright [2020] zhangke
#ifndef CLIENT_PROXY_CLIENT_H_
#define CLIENT_PROXY_CLIENT_H_

#include <muduo/base/Condition.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpClient.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/message_dispatch.h"
#include "tcp_client.h"

enum class ProxyConnState : uint32_t {
  INIT_STATE,
  CONNECTING,
  CONNECTED,
};

struct ProxyConnection {
  uint64_t conn_key;
  std::unique_ptr<TcpClient> client_conn;
  ProxyConnState state;
  bool server_open;  // 连接server是否成功
  bool client_open;  // client是否连接
  MessagePtr connect_request;
  std::vector<std::string> pending_data;
  bool client_block;
};

class ProxyClient : public std::enable_shared_from_this<ProxyClient> {
 public:
  ProxyClient(muduo::net::EventLoop *loop,
              const muduo::net::InetAddress &server_address,
              const muduo::net::InetAddress &local_address,
              uint16_t listen_port)
      : loop_(loop),
        dispatcher_(new MessageDispatch(loop_)),
        source_entity_(0),
        server_address_(server_address),
        local_address_(local_address),
        listen_port_(listen_port),
        start_finish_(false),
        start_retcode_(0),
        cond_(mutex_),
        session_key_(0),
        first_connect_(true) {}
  int Start();
  void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                 muduo::net::Buffer *buf, muduo::Timestamp time) {
    dispatcher_->OnMessage(conn, buf, time);
  }
  void OnProxyConnection(const muduo::net::TcpConnectionPtr &);
  void OnNewConnection(const muduo::net::TcpConnectionPtr &conn,
                       ProxyMessagePtr request_head, MessagePtr message);
  void OnNewData(const muduo::net::TcpConnectionPtr &conn,
                 ProxyMessagePtr message);
  void OnCloseConnection(const muduo::net::TcpConnectionPtr &conn,
                         ProxyMessagePtr request_head, MessagePtr message);
  void HandleListenResponse(MessagePtr message);
  void OnClientConnection(const muduo::net::TcpConnectionPtr &,
                          uint64_t conn_key, ProxyMessagePtr request_head);
  void OnClientMessage(const muduo::net::TcpConnectionPtr &,
                       muduo::net::Buffer *buffer, muduo::Timestamp);
  void OnClientClose(const muduo::net::TcpConnectionPtr &, uint64_t conn_key);
  void HandleDataResponse(const muduo::net::TcpConnectionPtr &conn,
                          ProxyMessagePtr response);
  void HandleCloseResponse(MessagePtr message, uint64_t conn_key);
  void EntryPauseSend(MessagePtr, uint64_t conn_id);
  void EntryResumeSend(MessagePtr, uint64_t conn_id);
  void HandlePauseSendRequest(const muduo::net::TcpConnectionPtr,
                              ProxyMessagePtr request_head, MessagePtr message);
  void HandleResumeSendRequest(const muduo::net::TcpConnectionPtr,
                               ProxyMessagePtr request_head,
                               MessagePtr message);
  void HandleHeartbeat(const muduo::net::TcpConnectionPtr,
                       ProxyMessagePtr request_head, MessagePtr message);
  void EntryHeartBeat(MessagePtr message);

 private:
  std::shared_ptr<ProxyClient> this_ptr() { return shared_from_this(); }
  void StartProxyService();
  uint32_t GetSourceEntity() { return ++source_entity_; }
  void ClientClose(uint64_t conn_key);
  void RemoveConnection(uint64_t conn_key, bool destroy = true);
  void StopClientRead(uint64_t conn_id = 0, bool client_block = false);
  void ResumeClientRead(uint64_t conn_id = 0, bool client_block = false);
  void OnHighWaterMark(bool is_proxy_conn, const muduo::net::TcpConnectionPtr &,
                       size_t);
  void OnWriteComplete(bool is_proxy_conn,
                       const muduo::net::TcpConnectionPtr &);
  void SendHeartBeat();
  muduo::net::EventLoop *loop_;
  std::unique_ptr<MessageDispatch> dispatcher_;
  uint32_t source_entity_;
  muduo::net::InetAddress server_address_;
  muduo::net::InetAddress local_address_;
  uint16_t listen_port_;
  bool start_finish_;
  int start_retcode_;
  muduo::MutexLock mutex_;
  muduo::Condition cond_ GUARDED_BY(mutex_);
  std::unique_ptr<muduo::net::TcpClient> proxy_client_;
  uint64_t session_key_;
  std::unordered_map<uint64_t, ProxyConnection> clients_;
  bool first_connect_;
  std::once_flag start_flag_;
  muduo::net::TimerId heartbeat_timer_;
};

#endif  // CLIENT_PROXY_CLIENT_H_
