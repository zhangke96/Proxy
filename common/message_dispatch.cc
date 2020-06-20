// Copyright [2020] zhangke

#include "common/message_dispatch.h"

#include <muduo/base/Logging.h>

#include <memory>
#include <string>
#include <utility>

MessageDispatch::MessageDispatch(muduo::net::EventLoop *loop)
    : loop_(loop), request_id_(0) {}

MessageDispatch::~MessageDispatch() { loop_->cancel(check_timeout_timerid); }

void MessageDispatch::Init() {
  // 注册pb response处理
  RegisterMsgHandle(PROTOBUF_MESSAGE,
                    std::bind(&MessageDispatch::OnPbMessage, this,
                              std::placeholders::_1, std::placeholders::_2));
  check_timeout_timerid =
      loop_->runEvery(0.01, std::bind(&MessageDispatch::CheckTimeout, this));
}

void MessageDispatch::CheckTimeout() {
  for (std::map<uint32_t, RequestContext>::iterator index =
           response_handles_.begin();
       index != response_handles_.end();) {
    auto &request_context = index->second;
    --request_context.timeout_count;
    if (request_context.timeout_count == 0) {
      if (request_context.retry_count == 0) {
        LOG_ERROR << "message timeout, request_id:" << index->first;
        if (request_context.timeout_cb) {
          request_context.timeout_cb();
        }
        index = response_handles_.erase(index);
        continue;
      } else {
        --(request_context.retry_count);
        LOG_INFO << "retry request_id: " << index->first << " send to:"
                  << (index->second).conn->peerAddress().toIpPort();
        (index->second)
            .conn->send(request_context.request.c_str(),
                        request_context.request.length());
      }
    }
    ++index;
  }

  for (std::map<uint32_t, PbRequestContext>::iterator index =
           pb_response_handles_.begin();
       index != pb_response_handles_.end();) {
    auto &request_context = index->second;
    --request_context.timeout_count;
    if (request_context.timeout_count == 0) {
      LOG_ERROR << "pb message timeout, source_id:" << index->first;
      if (request_context.timeout_cb) {
        request_context.timeout_cb();
      }
      index = pb_response_handles_.erase(index);
      continue;
    }
    ++index;
  }
}

void MessageDispatch::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                                muduo::net::Buffer *buf, muduo::Timestamp) {
  for (;;) {
    ProxyMessagePtr message_head_ptr(std::make_shared<ProxyMessage>());
    if (message_head_ptr->ParseFromBuffer(buf) == false) {
      break;
    } else {
      buf->retrieve(message_head_ptr->Size());
    }
    if (message_head_ptr->message_type % 2 == 0) {
      // response
      auto index = response_handles_.find(message_head_ptr->request_id);
      if (index == response_handles_.end()) {
        LOG_ERROR << "message resp not found, request_id:"
                  << message_head_ptr->request_id << " maybe timeout";
      } else {
        LOG_TRACE << "response from:" << conn->peerAddress().toIpPort()
                  << " request_id:" << message_head_ptr->request_id;
        (index->second).response_cb(conn, message_head_ptr);
        double used_time_ms =
            muduo::timeDifference(muduo::Timestamp::now(),
                                  (index->second).send_timestamp) *
            1000;
        LOG_TRACE << "message_type:" << message_head_ptr->message_type
                  << " request length:" << (index->second).request.size()
                  << " response length:" << message_head_ptr->Size()
                  << " used time(ms):" << used_time_ms;
        response_handles_.erase(index);
      }
    } else {
      // request
      auto index = msg_handlers_.find(message_head_ptr->message_type);
      if (index == msg_handlers_.end()) {
        LOG_ERROR << "unregister message_type:"
                  << message_head_ptr->message_type;
      } else {
        (index->second)(conn, message_head_ptr);
      }
    }
  }
}

void MessageDispatch::OnPbMessage(const muduo::net::TcpConnectionPtr &conn,
                                  ProxyMessagePtr message) {
  PbRequestBody *pb_message_body = dynamic_cast<PbRequestBody *>(message->body);
  assert(pb_message_body);
  MessagePtr pb_message(std::make_shared<proto::Message>());
  bool parse_ret = pb_message->ParseFromString(pb_message_body->data);
  if (!parse_ret) {
    LOG_ERROR << "Parse pb error";
    return;
  }
  if (pb_message->head().has_dest_entity()) {
    uint32_t dest_entity = pb_message->head().dest_entity();
    auto index = pb_response_handles_.find(dest_entity);
    if (index == pb_response_handles_.end()) {
      LOG_ERROR << "message resp not found, randon_num:"
                << pb_message->head().random_num()
                << " flow_num:" << pb_message->head().flow_no()
                << " desst_entity:" << pb_message->head().dest_entity();
    } else {
      LOG_DEBUG << "pb response from:" << conn->peerAddress().toIpPort() << "\n"
                << pb_message->DebugString();
      (index->second).response_cb(pb_message);
      pb_response_handles_.erase(index);
    }
  } else {
    // 查找请求处理函数
    uint32_t message_type = pb_message->head().message_type();
    auto index = pb_register_handles_.find(message_type);
    if (index == pb_register_handles_.end()) {
      LOG_ERROR << "message_type:" << message_type << " handle not registed";
    } else {
      LOG_DEBUG << "pb request from:" << conn->peerAddress().toIpPort() << "\n"
                << pb_message->DebugString();
      (index->second)(conn, message, pb_message);
    }
  }
}

void MessageDispatch::OnPbMessageTimeout(uint32_t source_entity) {
  auto index = pb_response_handles_.find(source_entity);
  if (index == pb_response_handles_.end()) {
    LOG_ERROR << "pb message timeout, not found source_id:" << source_entity;
    return;
  } else {
    LOG_ERROR << "pb message timeout, source_id:" << source_entity;
    if (index->second.timeout_cb) {
      (index->second).timeout_cb();
    }
    pb_response_handles_.erase(index);
  }
}

void MessageDispatch::RegisterPbHandle(int32_t message_type,
                                       HandleFunction handle_function) {
  pb_register_handles_[message_type] = std::move(handle_function);
}

void MessageDispatch::RegisterMsgHandle(uint16_t message_type,
                                        MsgHandleFunction handle_function) {
  msg_handlers_[message_type] = std::move(handle_function);
}

void MessageDispatch::SendPbRequest(const muduo::net::TcpConnectionPtr &conn,
                                    MessagePtr message,
                                    PbResponseCb response_cb,
                                    TimeoutCb timeout_cb, double timeout,
                                    uint16_t retry_count) {
  std::string message_str;
  bool serialize_ret = message->SerializeToString(&message_str);
  if (!serialize_ret) {
    LOG_ERROR << "serialize message failed";
    return;
  }
  PbRequestContext request_context;
  request_context.timeout_count = (timeout * (retry_count + 1) + 1) * 100;
  request_context.response_cb = std::move(response_cb);
  request_context.timeout_cb = std::move(timeout_cb);
  uint32_t source_entity = message->head().source_entity();
  pb_response_handles_[source_entity] = request_context;
  PbRequestBody request_body;
  request_body.length = message_str.length();
  request_body.data = std::move(message_str);
  ProxyMessage request_head;
  request_head.message_type = PROTOBUF_MESSAGE;
  request_head.length = request_body.Size();
  request_head.request_id = GetRequestId();
  request_head.body = &request_body;
  LOG_DEBUG << "pb request to:" << conn->peerAddress().toIpPort() << "\n"
            << message->DebugString();
  SendRequest(
      conn, &request_head,
      std::bind(&MessageDispatch::OnPbMessage, this, std::placeholders::_1,
                std::placeholders::_2),
      std::bind(&MessageDispatch::OnPbMessageTimeout, this, source_entity),
      timeout, retry_count);
  request_head.body = nullptr;
}

void MessageDispatch::SendRequest(const muduo::net::TcpConnectionPtr &conn,
                                  ProxyMessage *message,
                                  MsgHandleFunction response_cb,
                                  TimeoutCb timeout_cb, double timeout,
                                  uint16_t retry_count) {
  message->request_id = GetRequestId();
  RequestContext request_context;
  request_context.conn = conn;
  request_context.timeout_count = timeout * 100;
  request_context.retry_count = retry_count;
  request_context.request = message->ToString();
  request_context.send_timestamp = muduo::Timestamp::now();
  request_context.response_cb = std::move(response_cb);
  request_context.timeout_cb = std::move(timeout_cb);
  LOG_TRACE << "request_id:" << message->request_id
            << " send to:" << conn->peerAddress().toIpPort();
  conn->send(request_context.request.c_str(), request_context.request.length());
  response_handles_[message->request_id] = std::move(request_context);
}

void MessageDispatch::SendResponse(const muduo::net::TcpConnectionPtr &conn,
                                   const ProxyMessage *message) {
  std::string message_ptr = message->ToString();
  LOG_TRACE << "response to:" << conn->peerAddress().toIpPort();
  conn->send(message_ptr.c_str(), message_ptr.length());
}

void MessageDispatch::SendPbResponse(const muduo::net::TcpConnectionPtr &conn,
                                     ProxyMessagePtr request,
                                     MessagePtr message) {
  std::string message_str;
  bool serialize_ret = message->SerializeToString(&message_str);
  if (!serialize_ret) {
    LOG_ERROR << "serialize message failed";
    return;
  }
  PbResponseBody response_body;
  response_body.length = message_str.length();
  response_body.data = std::move(message_str);
  ProxyMessage response_head;
  response_head.message_type = PROTOBUF_RESPONSE;
  response_head.length = response_body.Size();
  response_head.request_id = request->request_id;
  response_head.body = &response_body;
  SendResponse(conn, &response_head);
  response_head.body = nullptr;
}
