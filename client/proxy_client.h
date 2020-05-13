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
#include <string>
#include <unordered_map>
#include <vector>

#include "common/pb_dispatch.h"

enum class ProxyConnState : uint32_t {
  INIT_STATE,
  CONNECTING,
  CONNECTED,
};

struct ProxyConnection {
  uint64_t conn_key;
  std::unique_ptr<muduo::net::TcpClient> client_conn;
  ProxyConnState state;
  bool client_open;
  bool server_open;
  MessagePtr connect_request;
  std::vector<std::string> pending_data;
};

class ProxyClient : public PbDispatch,
                    public std::enable_shared_from_this<ProxyClient> {
 public:
  ProxyClient(const muduo::net::InetAddress &server_address,
              const muduo::net::InetAddress &local_address,
              uint16_t listen_port)
      : source_entity_(0),
        server_address_(server_address),
        local_address_(local_address),
        listen_port_(listen_port),
        loop_(nullptr),
        start_finish_(false),
        start_retcode_(0),
        cond_(mutex_),
        session_key_(0) {}
  int Start();
  void OnProxyConnection(const muduo::net::TcpConnectionPtr &);
  void OnNewConnection(const muduo::net::TcpConnectionPtr &conn,
                       MessagePtr message);
  void OnNewData(const muduo::net::TcpConnectionPtr &conn, MessagePtr message);
  void OnCloseConnection(const muduo::net::TcpConnectionPtr &conn,
                         MessagePtr message);
  void HandleListenResponse(MessagePtr message);
  void OnClientConnection(const muduo::net::TcpConnectionPtr &,
                          uint64_t conn_key);
  void OnClientMessage(const muduo::net::TcpConnectionPtr &,
                       muduo::net::Buffer *buffer, muduo::Timestamp);
  void HandleDataResponse(MessagePtr message);

 private:
  std::shared_ptr<ProxyClient> this_ptr() { return shared_from_this(); }
  void StartProxyService();
  uint32_t GetSourceEntity() { return ++source_entity_; }
  uint32_t source_entity_;
  muduo::net::InetAddress server_address_;
  muduo::net::InetAddress local_address_;
  uint16_t listen_port_;
  muduo::net::EventLoop *loop_;
  std::unique_ptr<muduo::net::EventLoopThread> event_loop_thread_;
  bool start_finish_;
  int start_retcode_;
  muduo::MutexLock mutex_;
  muduo::Condition cond_ GUARDED_BY(mutex_);
  std::unique_ptr<muduo::net::TcpClient> proxy_client_;
  uint64_t session_key_;
  std::unordered_map<uint64_t, ProxyConnection> clients_;
};

#endif  // CLIENT_PROXY_CLIENT_H_
