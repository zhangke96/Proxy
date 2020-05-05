// Copyright 2020 zhangke
#ifndef COMMON_MESSAGE_UTIL_H_
#define COMMON_MESSAGE_UTIL_H_

#include <random>
#include "common/message.pb.h"

void MakeResponse(const proto::Message *req_msg,
                  proto::MessageType message_type, proto::Message *resp_msg);

void MakeMessage(proto::Message *msg, proto::MessageType message_type,
                 uint32_t source_entity, std::string auth_key = "",
                 uint64_t session_key = 0);

#endif  // COMMON_MESSAGE_UTIL_H_
