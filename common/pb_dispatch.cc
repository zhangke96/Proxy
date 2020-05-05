// Copyright [2020] zhangke

#include "common/pb_dispatch.h"
#include <muduo/base/Logging.h>
#include <string>
#include <utility>

PbDispatch::PbDispatch(muduo::net::EventLoop *loop) : loop_(loop) {}

void PbDispatch::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                           muduo::net::Buffer *buf, muduo::Timestamp) {
  while (buf->readableBytes() >= sizeof(uint32_t)) {
    uint32_t msg_length = static_cast<uint32_t>(buf->peekInt32());
    if (buf->readableBytes() >= msg_length) {
      buf->retrieveInt32();
      const char *message_ptr = buf->peek();
      std::string msg_str(message_ptr, msg_length);
      MessagePtr message(std::make_shared<proto::Message>());
      buf->retrieve(msg_length);
      bool parse_ret = message->ParseFromString(msg_str);
      if (!parse_ret) {
        LOG_ERROR << "Parse pb error";
        return;
      } else {
        if (message->head().has_dest_entity()) {
          uint32_t dest_entity = message->head().dest_entity();
          auto index = response_handles_.find(dest_entity);
          if (index == response_handles_.end()) {
            LOG_ERROR << "message resp not found, randon_num:"
                      << message->head().random_num()
                      << " flow_num:" << message->head().flow_no()
                      << " desst_entity:" << message->head().dest_entity();
          } else {
            (index->second)(message);
            response_handles_.erase(index);
          }
          return;
        }
        if (register_handles_.find(message->head().message_type()) !=
            register_handles_.end()) {
          register_handles_[message->head().message_type()](conn, message);
          return;
        } else {
          LOG_ERROR << "unreigster message_type:"
                    << message->head().message_type();
          return;
        }
      }
    }
  }
}

void PbDispatch::RegisterHandle(int32_t message_type,
                                HandleFunction handle_function) {
  register_handles_[message_type] = std::move(handle_function);
}

void PbDispatch::SendResponse(const muduo::net::TcpConnectionPtr &conn,
                              MessagePtr message) {
  std::string str;
  bool serialize_ret = message->SerializeToString(&str);
  if (!serialize_ret) {
    LOG_ERROR << "serialize fail," << message->DebugString();
    return;
  }
  uint32_t msg_length = str.size();
  msg_length = htonl(msg_length);
  conn->send(&msg_length, sizeof(msg_length));
  conn->send(str.c_str(), str.size());
}

void PbDispatch::SendRequest(const muduo::net::TcpConnectionPtr &conn,
                             MessagePtr message,
                             ResponseHandleFunction handler) {
  response_handles_[message->head().source_entity()] = std::move(handler);
  SendResponse(conn, message);
}
