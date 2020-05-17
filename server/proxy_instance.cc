// Copyright [2020] zhangke

#include <muduo/base/Logging.h>
#include <muduo/net/SocketsOps.h>
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
  RegisterHandle(proto::CLOSE_CONNECTION_REQUEST,
                 std::bind(&ProxyInstance::HandleCloseConnRequest, this,
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
  if (acceptor_) {
    // 已经有监听
    response_body->mutable_rc()->set_retcode(-1);
    response_body->mutable_rc()->set_error_message("already listen");
    SendResponse(proxy_conn_, listen_response_msg_);
    return;
  }
  uint16_t listen_port =
      static_cast<uint16_t>(message->body().listen_request().listen_port());
  listen_addr_ = muduo::net::InetAddress("0.0.0.0", listen_port);
  StartListen();
  response_body->mutable_rc()->set_retcode(0);
  response_body->mutable_rc()->set_error_message("success");
  SendResponse(proxy_conn_, listen_response_msg_);
  return;
}

void ProxyInstance::StartListen() {
  loop_->assertInLoopThread();
  acceptor_.reset(new muduo::net::Acceptor(loop_, listen_addr_, true));
  acceptor_->setNewConnectionCallback(std::bind(&ProxyInstance::OnNewConnection,
                                                this, std::placeholders::_1,
                                                std::placeholders::_2));
  thread_pool_.reset(
      new muduo::net::EventLoopThreadPool(loop_, "proxy_server"));
  thread_pool_->start(nullptr);
  acceptor_->listen();
}

void ProxyInstance::OnNewConnection(int sockfd,
                                    const muduo::net::InetAddress &peer_addr) {
  loop_->assertInLoopThread();
  muduo::net::EventLoop *io_loop = thread_pool_->getNextLoop();
  char conn_name[64];
  snprintf(conn_name, sizeof(conn_name), "%s--%s",
           listen_addr_.toIpPort().c_str(), peer_addr.toIpPort().c_str());
  muduo::net::InetAddress local_addr(muduo::net::sockets::getLocalAddr(sockfd));
  muduo::net::TcpConnectionPtr conn(std::make_shared<muduo::net::TcpConnection>(
      io_loop, conn_name, sockfd, local_addr, peer_addr));
  conn->setConnectionCallback(std::bind(&ProxyInstance::OnClientConnection,
                                        this, std::placeholders::_1));
  conn->setCloseCallback(
      std::bind(&ProxyInstance::OnClientClose, this, std::placeholders::_1));
  conn->setMessageCallback(
      std::bind(&ProxyInstance::OnClientMessage, this, std::placeholders::_1,
                std::placeholders::_2, std::placeholders::_3));
  io_loop->runInLoop(
      std::bind(&muduo::net::TcpConnection::connectEstablished, conn));
}

void ProxyInstance::OnClientConnection(
    const muduo::net::TcpConnectionPtr &conn) {
  loop_->runInLoop([=] {
    if (conn->getContext().empty()) {
      // 连接建立调用
      assert(conn->getContext().empty());
      uint64_t conn_id = GetConnId();
      conn->setContext(conn_id);
      MessagePtr message = std::make_shared<proto::Message>();
      MakeMessage(message.get(), proto::NEW_CONNECTION_REQUEST,
                  GetSourceEntity());
      proto::NewConnectionRequest *new_connection_request =
          message->mutable_body()->mutable_new_connection_request();
      new_connection_request->set_conn_key(conn_id);
      conn_map_.insert(std::make_pair(conn_id, Connection(conn)));
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
        for (uint i = 0;
             i < sizeof(address->sin6_addr.s6_addr) / sizeof(uint8_t); ++i) {
          *(new_connection_request->add_ip_v6()) =
              address->sin6_addr.s6_addr[i];
        }
        new_connection_request->set_port(ntohs(address->sin6_port));
      }
      SendRequest(proxy_conn_, message,
                  std::bind(&ProxyInstance::EntryAddConnection, this_ptr(),
                            std::placeholders::_1, conn));
    } else {
      // client shutdown write(recv fin)
      // 主动destroy连接
    }
  });
}

void ProxyInstance::OnClientMessage(const muduo::net::TcpConnectionPtr &conn,
                                    muduo::net::Buffer *buffer,
                                    muduo::Timestamp) {
  loop_->runInLoop([=] {
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    assert(conn_id);
    assert(conn_map_.count(conn_id));
    Connection &client_conn = conn_map_[conn_id];
    if (client_conn.proxy_accept) {
      MessagePtr message = std::make_shared<proto::Message>();
      MakeMessage(message.get(), proto::DATA_REQUEST, GetSourceEntity());
      proto::DataRequest *data_request =
          message->mutable_body()->mutable_data_request();
      data_request->set_conn_key(conn_id);
      std::string data_str(buffer->peek(), buffer->readableBytes());
      *(data_request->add_data()) = std::move(data_str);
      buffer->retrieve(buffer->readableBytes());
      SendRequest(proxy_conn_, message,
                  std::bind(&ProxyInstance::EntryData, this_ptr(),
                            std::placeholders::_1, conn));
    } else {
      LOG_DEBUG << "new data before proxy client accept connection, conn_id:"
                << conn_id;
      client_conn.pending_message.push_back(
          std::string(buffer->peek(), buffer->readableBytes()));
      buffer->retrieveAll();
    }
  });
}

void ProxyInstance::OnClientClose(const muduo::net::TcpConnectionPtr &conn) {
  // 更新状态
  loop_->runInLoop([=] {
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    assert(conn_id);
    assert(conn_map_.count(conn_id));
    Connection &client_conn = conn_map_[conn_id];
    assert(client_conn.client_close == false);
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::CLOSE_CONNECTION_REQUEST,
                GetSourceEntity());
    proto::CloseConnectionRequest *close_connection_request =
        message->mutable_body()->mutable_close_connection_request();
    close_connection_request->set_conn_key(conn_id);
    SendRequest(proxy_conn_, message,
                std::bind(&ProxyInstance::EntryCloseConnection, this_ptr(),
                          std::placeholders::_1, conn_id));
  });
}

void ProxyInstance::EntryCloseConnection(MessagePtr message, uint64_t conn_id) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_RESONSE);
  assert(message->body().has_close_connection_response());
  // const proto::CloseConnectionResponse &response =
  //     message->body().close_connection_response();
  // 忽略返回码
  assert(conn_map_.count(conn_id));
  conn_map_[conn_id].client_close = true;
  if (conn_map_[conn_id].server_close) {
    // 如果server也关闭了
    muduo::net::TcpConnectionPtr conn = conn_map_[conn_id].conn;
    RemoveConnecion(conn_id);
  }
}

void ProxyInstance::EntryAddConnection(
    MessagePtr message, const muduo::net::TcpConnectionPtr &client_conn) {
  assert(message->head().message_type() == proto::NEW_CONNECTION_RESPONSE);
  assert(message->body().has_new_connection_response());
  const proto::NewConnectionResponse &response =
      message->body().new_connection_response();
  uint64_t conn_id = boost::any_cast<uint64_t>(client_conn->getContext());
  // conn_map_[conn_id] = client_conn;
  if (response.rc().retcode() == 0) {
    Connection &conn = conn_map_[conn_id];
    conn.proxy_accept = true;
    if (!conn.pending_message.empty()) {
      muduo::net::Buffer buffer;
      for (const std::string &msg : conn.pending_message) {
        buffer.append(msg.c_str(), msg.length());
      }
      OnClientMessage(client_conn, &buffer, muduo::Timestamp::now());
      conn.pending_message.clear();
    }
  } else {
    LOG_ERROR << "proxy client accept connection fail";
    // 强制关闭客户端连接
    RemoveConnecion(conn_id);
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
  if (conn_map_.find(conn_key) != conn_map_.end()) {
    for (int i = 0; i < data_request.data_size(); ++i) {
      const std::string &data = data_request.data(i);
      (conn_map_[conn_key]).conn->send(data.c_str(), data.size());
    }
    response_body->mutable_rc()->set_retcode(0);
  } else {
    response_body->mutable_rc()->set_retcode(-1);
  }
  // response_body->
  SendResponse(proxy_conn_, data_response);
  return;
}

void ProxyInstance::HandleCloseConnRequest(const muduo::net::TcpConnectionPtr,
                                           MessagePtr message) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_REQUEST);
  assert(message->body().has_close_connection_request());
  const proto::CloseConnectionRequest &request =
      message->body().close_connection_request();
  uint64_t conn_key = request.conn_key();
  MessagePtr close_response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::CLOSE_CONNECTION_RESONSE,
               close_response.get());
  proto::CloseConnectionResponse *response_body =
      close_response->mutable_body()->mutable_close_connection_response();
  if (conn_map_.find(conn_key) != conn_map_.end()) {
    Connection &conn = conn_map_[conn_key];
    if (conn.server_close == false) {
      conn.server_close = true;
      if (conn.client_close == true) {
        RemoveConnecion(conn_key);
      } else {
        conn.conn->shutdown();
      }
    }
    response_body->mutable_rc()->set_retcode(0);
  } else {
    response_body->mutable_rc()->set_retcode(-1);
  }
  SendResponse(proxy_conn_, close_response);
}

void ProxyInstance::RemoveConnecion(uint64_t conn_id) {
  auto index = conn_map_.find(conn_id);
  assert(index != conn_map_.end());
  index->second.conn->getLoop()->runInLoop(std::bind(
      &muduo::net::TcpConnection::connectDestroyed, index->second.conn));
  conn_map_.erase(index);
}
