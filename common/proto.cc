// Copyright [2020] zhangke

#include "common/proto.h"

#include <assert.h>
#include <muduo/base/Logging.h>

uint16_t GetUint16(const char *str) {
  uint16_t result;
  memcpy(&result, str, sizeof(uint16_t));
  LOG_TRACE << "GetUint16 network:" << result << " host:" << ntohs(result);
  return ntohs(result);
}

uint32_t GetUint32(const char *str) {
  uint32_t result;
  memcpy(&result, str, sizeof(uint32_t));
  LOG_TRACE << "GetUint32 network:" << result << " host:" << ntohl(result);
  return ntohl(result);
}

int32_t Getint32(const char *str) {
  int32_t result;
  memcpy(&result, str, sizeof(uint32_t));
  LOG_TRACE << "Getint32 network:" << result << " host:" << ntohl(result);
  return ntohl(result);
}

uint64_t GetUint64(const char *str) {
  uint64_t result;
  memcpy(&result, str, sizeof(uint64_t));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  LOG_TRACE << "GetUint64 network:" << result << " host:" << be64toh(result);
  return be64toh(result);
#else
  LOG_TRACE << "GetUint64 network:" << result << " host:" << result;
  return result;
#endif
}

size_t ProxyMessage::Size() const {
  size_t size = 0;
  size += sizeof(message_type);
  size += sizeof(message_version);
  size += sizeof(length);
  size += sizeof(request_id);
  if (body) {
    size += body->Size();
  }
  return size;
}

bool ProxyMessage::ParseFromStr(const char *str, size_t str_length) {
  LOG_TRACE << "length: " << str_length << " message head size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  int pos = 0;
  message_type = GetUint16(str + pos);
  pos += sizeof(message_type);
  message_version = GetUint16(str + pos);
  pos += sizeof(message_version);
  length = GetUint32(str + pos);
  pos += sizeof(length);
  request_id = GetUint32(str + pos);
  pos += sizeof(request_id);
  bool body_parse_ret = false;
  switch (message_type) {
    case ERROR:
    case ECHO_REQUEST:
    case ECHO_RESPONSE: {
      break;
    }
    case PROTOBUF_MESSAGE: {
      body = new PbRequestBody();
      body_parse_ret = body->ParseFromStr(str + pos, str_length - pos);
      if (!body_parse_ret) {
        LOG_TRACE << "Parse body pb request failed";
        delete body;
        body = nullptr;
      } else {
        LOG_TRACE << "Parse body pb request succ";
      }
      break;
    }
    case PROTOBUF_RESPONSE: {
      body = new PbResponseBody();
      body_parse_ret = body->ParseFromStr(str + pos, str_length - pos);
      if (!body_parse_ret) {
        LOG_TRACE << "Parse body pb response failed";
        delete body;
        body = nullptr;
      } else {
        LOG_TRACE << "Parse body pb response succ";
      }
      break;
    }
    case DATA_REQUEST: {
      body = new DataRequestBody();
      body_parse_ret = body->ParseFromStr(str + pos, str_length - pos);
      if (!body_parse_ret) {
        LOG_TRACE << "Parse body data request failed";
        delete body;
        body = nullptr;
      } else {
        LOG_TRACE << "Parse body data request succ";
      }
      break;
    }
    case DATA_RESPONSE: {
      body = new DataResponseBody();
      body_parse_ret = body->ParseFromStr(str + pos, str_length - pos);
      if (!body_parse_ret) {
        LOG_TRACE << "Parse body data response failed";
        delete body;
        body = nullptr;
      } else {
        LOG_TRACE << "Parse body data response succ";
      }
      break;
    }
    default: {
      LOG_TRACE << "unhandle message type:" << message_type;
      break;
    }
  }
  return body_parse_ret;
}

std::string ProxyMessage::ToString() const {
  std::string result;
  result.reserve(Size());
  uint16_t message_type_n = htons(message_type);
  result.append(reinterpret_cast<char *>(&message_type_n),
                sizeof(message_type_n));
  uint16_t message_version_n = htons(message_version);
  result.append(reinterpret_cast<char *>(&message_version_n),
                sizeof(message_version_n));
  uint32_t length_n = htonl(length);
  result.append(reinterpret_cast<char *>(&length_n), sizeof(length_n));
  uint32_t request_id_n = htonl(request_id);
  result.append(reinterpret_cast<char *>(&request_id_n), sizeof(request_id_n));
  LOG_TRACE << "ProxyMessage total size:" << Size()
            << " message_type:" << message_type << " body length:" << length
            << " request_id:" << request_id;
  if (body) {
    LOG_TRACE << "actual body length:" << body->Size();
    result.append(body->ToString());
  }
  return result;
}

size_t PbRequestBody::Size() const { return sizeof(length) + length; }

bool PbRequestBody::ParseFromStr(const char *str, size_t str_length) {
  LOG_TRACE << "length: " << str_length << " minimal size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  int pos = 0;
  length = GetUint32(str + pos);
  pos += sizeof(length);
  LOG_TRACE << "length: " << str_length << " actual need size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  data = std::string(str + pos, length);
  return true;
}

std::string PbRequestBody::ToString() const {
  std::string result;
  assert(data.length() == length);
  result.reserve(Size());
  uint32_t length_n = htonl(length);
  result.append(reinterpret_cast<char *>(&length_n), sizeof(length_n));
  result.append(data);
  LOG_TRACE << "PbRequest[Response]Body total size:" << Size()
            << " length:" << length << " data length:" << data.length();
  return result;
}

size_t DataRequestBody::Size() const {
  return sizeof(length) + sizeof(conn_key) + length;
}

bool DataRequestBody::ParseFromStr(const char *str, size_t str_length) {
  LOG_TRACE << "length: " << str_length << " minimal size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  int pos = 0;
  length = GetUint32(str + pos);
  pos += sizeof(length);
  conn_key = GetUint64(str + pos);
  pos += sizeof(conn_key);
  LOG_TRACE << "length: " << str_length << " actual need size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  data = std::string(str + pos, length);
  return true;
}

std::string DataRequestBody::ToString() const {
  std::string result;
  assert(data.length() == length);
  result.reserve(Size());
  uint32_t length_n = htonl(length);
  result.append(reinterpret_cast<char *>(&length_n), sizeof(length_n));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint64_t conn_key_n = htobe64(conn_key);
#else
  uint64_t conn_key_n = conn_key;
#endif
  result.append(reinterpret_cast<char *>(&conn_key_n), sizeof(conn_key_n));
  result.append(data);
  LOG_TRACE << "DataRequestBody total size:" << Size() << " length:" << length
            << " conn_key:" << conn_key << " data length:" << data.length();
  return result;
}

size_t DataResponseBody::Size() const {
  return sizeof(length) + sizeof(retcode) + length;
}

bool DataResponseBody::ParseFromStr(const char *str, size_t str_length) {
  LOG_TRACE << "length: " << str_length << " minimal size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  int pos = 0;
  length = GetUint32(str + pos);
  pos += sizeof(length);
  retcode = Getint32(str + pos);
  pos += sizeof(retcode);
  LOG_TRACE << "length: " << str_length << " actual need size:" << Size();
  if (str_length < Size()) {
    return false;
  }
  data = std::string(str + pos, length);
  return true;
}

std::string DataResponseBody::ToString() const {
  std::string result;
  assert(data.length() == length);
  result.reserve(Size());
  uint32_t length_n = htonl(length);
  result.append(reinterpret_cast<char *>(&length_n), sizeof(length_n));
  uint32_t retcode_n = htonl(retcode);
  result.append(reinterpret_cast<char *>(&retcode_n), sizeof(retcode_n));
  result.append(data);
  LOG_TRACE << "DataResponseBody total size:" << Size() << " length:" << length
            << " retcode:" << retcode << " data length:" << data.length();
  return result;
}
