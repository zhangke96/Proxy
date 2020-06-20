// Copyright [2020] zhangke
#ifndef COMMON_MESSAGE_DISPATCH_H_
#define COMMON_MESSAGE_DISPATCH_H_

#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>

#include <map>
#include <memory>
#include <string>

#include "common/message.pb.h"
#include "common/proto.h"

typedef std::shared_ptr<proto::Message> MessagePtr;
typedef std::function<void(const muduo::net::TcpConnectionPtr &conn,
                           ProxyMessagePtr request_head, MessagePtr message)>
    HandleFunction;
typedef std::function<void(const muduo::net::TcpConnectionPtr &conn,
                           ProxyMessagePtr)>
    MsgHandleFunction;
typedef std::function<void(MessagePtr response)> PbResponseCb;
typedef std::function<void()> TimeoutCb;

struct RequestContext {
  muduo::net::TcpConnectionPtr conn;
  // 0.01s自减
  uint32_t timeout_count;
  uint16_t retry_count;
  std::string request;
  muduo::Timestamp send_timestamp;
  MsgHandleFunction response_cb;
  TimeoutCb timeout_cb;
};

struct ResponseContext {
  muduo::net::TcpConnectionPtr conn;
  ProxyMessage request_head;
};

struct PbRequestContext {
  // 这里超时时间要比RequestContext中大，用于消息无法解析时的超时处理
  uint32_t timeout_count;
  PbResponseCb response_cb;
  TimeoutCb timeout_cb;
};

class MessageDispatch {
 public:
  explicit MessageDispatch(muduo::net::EventLoop *loop);
  ~MessageDispatch();
  void Init();
  virtual void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                         muduo::net::Buffer *buf, muduo::Timestamp);
  void OnPbMessage(const muduo::net::TcpConnectionPtr &conn,
                   ProxyMessagePtr message);
  void RegisterPbHandle(int32_t message_type, HandleFunction);
  void RegisterMsgHandle(uint16_t message_type, MsgHandleFunction);
  virtual void SendPbRequest(const muduo::net::TcpConnectionPtr &conn,
                             MessagePtr message, PbResponseCb, TimeoutCb,
                             double timeout = 5.0, uint16_t retry_count = 0);
  virtual void SendRequest(const muduo::net::TcpConnectionPtr &conn,
                           ProxyMessage *message, MsgHandleFunction, TimeoutCb,
                           double timeout = 5.0, uint16_t retry_count = 0);
  virtual void SendResponse(const muduo::net::TcpConnectionPtr &conn,
                            const ProxyMessage *message);
  virtual void SendPbResponse(const muduo::net::TcpConnectionPtr &conn,
                              ProxyMessagePtr request, MessagePtr message);

 private:
  void CheckTimeout();
  void OnPbMessageTimeout(uint32_t source_entity);
  uint32_t GetRequestId() { return ++request_id_; }

  muduo::net::EventLoop *loop_;
  std::map<int32_t, HandleFunction> pb_register_handles_;
  std::map<uint16_t, MsgHandleFunction> msg_handlers_;
  // request_id => response_handle
  std::map<uint32_t, RequestContext> response_handles_;
  std::map<uint32_t, PbRequestContext> pb_response_handles_;
  uint32_t request_id_;
  muduo::net::TimerId check_timeout_timerid;
};

#endif  // COMMON_MESSAGE_DISPATCH_H_
