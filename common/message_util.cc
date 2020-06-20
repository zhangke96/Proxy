// Copyright [2020] zhangke

#include "common/message_util.h"

#include <stdint.h>

#include <atomic>

void MakeResponse(const proto::Message *req_msg,
                  proto::MessageType message_type, proto::Message *resp_msg) {
  const proto::Head &req_head = req_msg->head();
  proto::Head *resp_head = resp_msg->mutable_head();
  resp_head->set_version(req_head.version());
  resp_head->set_random_num(req_head.random_num());
  resp_head->set_flow_no(req_head.flow_no());
  resp_head->set_message_type(message_type);
  resp_head->set_source_entity(0);
  resp_head->set_dest_entity(req_head.source_entity());
}

std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<uint32_t> dis(1, UINT32_MAX);

std::atomic<uint32_t> flow_no;

uint32_t FlowNo() {
  ++flow_no;
  if (flow_no == UINT32_MAX) {
    flow_no = 0;
  }
  return flow_no;
}

void MakeMessage(proto::Message *msg, proto::MessageType message_type,
                 uint32_t source_entity, std::string auth_key,
                 uint64_t session_key) {
  proto::Head *head = msg->mutable_head();
  head->set_version(1);
  head->set_random_num(dis(gen));
  head->set_flow_no(FlowNo());
  head->set_message_type(message_type);
  head->set_source_entity(source_entity);
  if (!auth_key.empty()) {
    head->set_auth_key(auth_key);
  }
  if (session_key) {
    head->set_session_key(session_key);
  }
}