// Copyright [2020] zhangke
#ifndef COMMON_PB_DISPATCH_H_
#define COMMON_PB_DISPATCH_H_

#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>
#include <map>
#include <memory>
#include "common/message.pb.h"

typedef std::shared_ptr<proto::Message> MessagePtr;
typedef std::function<void(const muduo::net::TcpConnectionPtr &conn,
                           MessagePtr message)> HandleFunction;
typedef std::function<void(MessagePtr message)> ResponseHandleFunction;

class PbDispatch {
 public:
  virtual void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                         muduo::net::Buffer *buf, muduo::Timestamp);
  void RegisterHandle(int32_t message_type, HandleFunction);
  virtual void SendRequest(const muduo::net::TcpConnectionPtr &conn,
                           MessagePtr message, ResponseHandleFunction);
  virtual void SendResponse(const muduo::net::TcpConnectionPtr &conn,
                            MessagePtr message);

 private:
  std::map<int32_t, HandleFunction> register_handles_;
  std::map<uint32_t, ResponseHandleFunction> response_handles_;
};

#endif  // COMMON_PB_DISPATCH_H_
