// Copyright [2020] zhangke
#ifndef COMMON_PROTO_H_
#define COMMON_PROTO_H_
#include <arpa/inet.h>
#include <endian.h>
#include <muduo/net/Buffer.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>

uint16_t GetUint16(const char *str);

uint32_t GetUint32(const char *str);

int32_t Getint32(const char *str);

uint64_t GetUint64(const char *str);

enum ProxyMsgType : uint16_t {
  ERROR = 0,
  ECHO_REQUEST = 1,
  ECHO_RESPONSE = 2,
  PROTOBUF_MESSAGE = 3,
  PROTOBUF_RESPONSE = 4,
  DATA_REQUEST = 5,
  DATA_RESPONSE = 6,
  MAX_MSGTYPE,
};

// struct ProxyMsgHead {
//   uint32_t length;  // 数据的长度
//   ProxyMsgCommand command;
//   char data[];
// };

struct MessageBase {
  virtual ~MessageBase() {}
  virtual size_t Size() const = 0;
  virtual bool ParseFromStr(const char *str, size_t str_length) = 0;
  bool ParseFromBuffer(muduo::net::Buffer *buf) {
    return ParseFromStr(buf->peek(), buf->readableBytes());
  }
  bool ParseFromStr(const std::string &str) {
    return ParseFromStr(str.c_str(), str.length());
  }
  virtual std::string ToString() const = 0;
};

struct ProxyMessage : public MessageBase {
  ProxyMessage()
      : message_type(0),
        message_version(0),
        length(0),
        request_id(0),
        body(nullptr) {}

  ProxyMessage(const ProxyMessage &) = delete;

  ProxyMessage &operator=(const ProxyMessage &) = delete;

  ProxyMessage(ProxyMessage &&rhs) {
    message_type = rhs.message_type;
    message_version = rhs.message_version;
    length = rhs.length;
    request_id = rhs.request_id;
    body = rhs.body;
  }

  ProxyMessage &operator=(ProxyMessage &&rhs) noexcept {
    if (this != &rhs) {
      delete body;
      message_type = rhs.message_type;
      message_version = rhs.message_version;
      length = rhs.length;
      request_id = rhs.request_id;
      body = rhs.body;
    }
  }

  ~ProxyMessage() { delete body; }

  uint16_t message_type;
  uint16_t message_version;
  uint32_t length;
  uint32_t request_id;
  MessageBase *body;
  size_t Size() const override;
  bool ParseFromStr(const char *str, size_t str_length) override;
  std::string ToString() const override;
};

struct PbRequestBody : public MessageBase {
  PbRequestBody() : length(0) {}
  uint32_t length;
  std::string data;
  size_t Size() const override;
  bool ParseFromStr(const char *str, size_t str_length) override;
  std::string ToString() const override;
};

typedef PbRequestBody PbResponseBody;

struct DataRequestBody : public MessageBase {
  DataRequestBody() : length(0), conn_key(0) {}
  uint32_t length;
  uint64_t conn_key;
  std::string data;
  size_t Size() const override;
  bool ParseFromStr(const char *str, size_t str_length) override;
  std::string ToString() const override;
};

enum DataResponseCode : int32_t {
  SUCCESS = 0,
  FAIL = -1,
};

struct DataResponseBody : public MessageBase {
  DataResponseBody() : length(0), retcode(0) {}
  uint32_t length;
  int32_t retcode;
  std::string data;
  size_t Size() const override;
  bool ParseFromStr(const char *str, size_t str_length) override;
  std::string ToString() const override;
};

// namespace pkg {
// struct head {
//   uint32_t cmd;
//   uint32_t user;
//   uint32_t lengthOfAllPkg;
//   struct exinfo {
//     lengthOfExInfo tlv[0] { type length value }
//   };
// };
// }
// bodyBuf
// }

typedef std::shared_ptr<ProxyMessage> ProxyMessagePtr;

#endif  // COMMON_PROTO_H_
