// Copyright [2020] zhangke

#include <muduo/base/Logging.h>
#include <memory>
#include <string>
#include <utility>

#include "common/message_util.h"
#include "server/proxy_instance.h"

ProxyInstance::ProxyInstance(muduo::net::EventLoop *loop,
                             const muduo::net::TcpConnectionPtr &conn)
    : PbDispatch(),
      loop_(loop),
      proxy_conn_(conn),
      conn_id_(0),
      source_entity_(0) {}

void ProxyInstance::Init() {
  RegisterHandle(proto::LISTEN_REQUEST,
                 std::bind(&ProxyInstance::HandleListenRequest, this,
                           std::placeholders::_1, std::placeholders::_2));
  RegisterHandle(proto::DATA_REQUEST,
                 std::bind(&ProxyInstance::HandleDataRequest, this,
                           std::placeholders::_1, std::placeholders::_2));
}

void ProxyInstance::HandleListenRequest(const muduo::net::TcpConnectionPtr,
                                        MessagePtr message) {
  // 判断auth
  assert(message->head().message_type() == proto::LISTEN_REQUEST);
  assert(message->body().has_listen_request());
  listen_response_msg_ = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::LISTEN_RESPONSE,
               listen_response_msg_.get());
  proto::ListenResponse *response_body =
      listen_response_msg_->mutable_body()->mutable_listen_response();
  if (server_) {
    // 已经有监听
    response_body->mutable_rc()->set_retcode(-1);
    response_body->mutable_rc()->set_error_message("already listen");
    SendResponse(proxy_conn_, listen_response_msg_);
    return;
  }
  uint16_t listen_port =
      static_cast<uint16_t>(message->body().listen_request().listen_port());
  muduo::net::InetAddress client_listen_address("0.0.0.0", listen_port);
  server_.reset(
      new muduo::net::TcpServer(loop_, client_listen_address, "client_server"));
  server_->setConnectionCallback(std::bind(&ProxyInstance::OnClientConnection,
                                           this, std::placeholders::_1));
  server_->setMessageCallback(
      std::bind(&ProxyInstance::OnClientMessage, this, std::placeholders::_1,
                std::placeholders::_2, std::placeholders::_3));
  server_->start();
  response_body->mutable_rc()->set_retcode(0);
  // response_body->
  SendResponse(proxy_conn_, listen_response_msg_);
  return;
}

void ProxyInstance::OnClientConnection(
    const muduo::net::TcpConnectionPtr &conn) {
  if (conn->getContext().empty()) {
    uint64_t conn_id = GetConnId();
    conn->setContext(conn_id);
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::NEW_CONNECTION_REQUEST,
                GetSourceEntity());
    proto::NewConnectionRequest *new_connection_request =
        message->mutable_body()->mutable_new_connection_request();
    new_connection_request->set_conn_key(conn_id);
    muduo::net::InetAddress peer_address = conn->peerAddress();
    if (peer_address.family() == AF_INET) {
      const struct sockaddr_in *address =
          reinterpret_cast<const struct sockaddr_in *>(
              peer_address.getSockAddr());
      new_connection_request->set_ip_v4(ntohl(address->sin_addr.s_addr));
      new_connection_request->set_port(ntohs(address->sin_port));
    } else if (peer_address.family() == AF_INET6) {
      const struct sockaddr_in6 *address =
          reinterpret_cast<const struct sockaddr_in6 *>(
              peer_address.getSockAddr());
      for (uint i = 0; i < sizeof(address->sin6_addr.s6_addr) / sizeof(uint8_t);
           ++i) {
        *(new_connection_request->add_ip_v6()) = address->sin6_addr.s6_addr[i];
      }
      new_connection_request->set_port(ntohs(address->sin6_port));
    }
    SendRequest(proxy_conn_, message,
                std::bind(&ProxyInstance::EntryAddConnection, this_ptr(),
                          std::placeholders::_1, conn));
  } else {
    // 链接关闭
  }
}

void ProxyInstance::OnClientMessage(const muduo::net::TcpConnectionPtr &conn,
                                    muduo::net::Buffer *buffer,
                                    muduo::Timestamp) {
  uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
  assert(conn_id);
  if (conn_map_.find(conn_id) != conn_map_.end()) {
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::DATA_REQUEST, GetSourceEntity());
    proto::DataRequest *data_request =
        message->mutable_body()->mutable_data_request();
    data_request->set_conn_key(conn_id);
    std::string data_str(buffer->peek(), buffer->readableBytes());
    data_request->set_data(data_str);
    buffer->retrieve(buffer->readableBytes());
    SendRequest(proxy_conn_, message,
                std::bind(&ProxyInstance::EntryData, this_ptr(),
                          std::placeholders::_1, conn));
  } else {
    LOG_DEBUG << "new data before proxy client accept connection, conn_id:"
              << conn_id;
    pending_message_[conn_id].push_back(
        std::string(buffer->peek(), buffer->readableBytes()));
    buffer->retrieveAll();
  }
}

void ProxyInstance::EntryAddConnection(
    MessagePtr message, const muduo::net::TcpConnectionPtr &client_conn) {
  assert(message->head().message_type() == proto::NEW_CONNECTION_RESPONSE);
  assert(message->body().has_new_connection_response());
  const proto::NewConnectionResponse &response =
      message->body().new_connection_response();
  uint64_t conn_id = boost::any_cast<uint64_t>(client_conn->getContext());
  conn_map_[conn_id] = client_conn;
  if (response.rc().retcode() == 0) {
    if (pending_message_.count(conn_id)) {
      const std::vector<std::string> to_send_msg = pending_message_[conn_id];
      muduo::net::Buffer buffer;
      for (const std::string &msg : to_send_msg) {
        buffer.append(msg.c_str(), msg.length());
      }
      OnClientMessage(client_conn, &buffer, muduo::Timestamp::now());
      pending_message_.erase(conn_id);
    }
  } else {
    LOG_ERROR << "proxy client accept connection fail";
    pending_message_.erase(conn_id);
    client_conn->forceClose();
  }
}

void ProxyInstance::EntryData(MessagePtr message,
                              const muduo::net::TcpConnectionPtr &client_conn) {
  assert(message->head().message_type() == proto::DATA_RESPONSE);
  assert(message->body().has_data_response());
  uint64_t conn_id = boost::any_cast<uint64_t>(client_conn->getContext());
  const proto::DataResponse &response = message->body().data_response();
  if (response.rc().retcode() == 0) {
    LOG_TRACE << "conn:" << conn_id << " recv data succ";
  } else {
    conn_map_.erase(conn_id);
    client_conn->forceClose();
    LOG_WARN << "conn:" << conn_id << " force close";
  }
}

uint64_t ProxyInstance::GetConnId() { return ++conn_id_; }

uint32_t ProxyInstance::GetSourceEntity() { return ++source_entity_; }

void ProxyInstance::HandleDataRequest(const muduo::net::TcpConnectionPtr,
                                      MessagePtr message) {
  // 判断auth
  assert(message->head().message_type() == proto::DATA_REQUEST);
  assert(message->body().has_data_request());
  MessagePtr data_response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::DATA_RESPONSE, data_response.get());
  proto::DataResponse *response_body =
      data_response->mutable_body()->mutable_data_response();
  const proto::DataRequest &data_request = message->body().data_request();
  uint64_t conn_key = data_request.conn_key();
  const std::string &data = data_request.data();
  if (conn_map_.find(conn_key) != conn_map_.end()) {
    conn_map_[conn_key]->send(data.c_str(), data.size());
    response_body->mutable_rc()->set_retcode(0);
  } else {
    response_body->mutable_rc()->set_retcode(-1);
  }
  // response_body->
  SendResponse(proxy_conn_, data_response);
  return;
}
