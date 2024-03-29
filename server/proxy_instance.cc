// Copyright [2020] zhangke

#include "server/proxy_instance.h"

#include <muduo/base/Logging.h>
#include <muduo/net/SocketsOps.h>

#include <memory>
#include <string>
#include <utility>

#include "common/message_util.h"
#include "common/proto.h"

ProxyInstance::ProxyInstance(muduo::net::EventLoop *loop,
                             const muduo::net::TcpConnectionPtr &conn)
    : loop_(loop),
      dispatcher_(new MessageDispatch(loop_)),
      proxy_conn_(conn),
      conn_id_(0),
      source_entity_(0),
      proxy_client_connect_(true) {}

ProxyInstance::~ProxyInstance() {}

void ProxyInstance::Init() {
  dispatcher_->Init();
  dispatcher_->RegisterPbHandle(
      proto::LISTEN_REQUEST,
      std::bind(&ProxyInstance::HandleListenRequest, this,
                std::placeholders::_1, std::placeholders::_2,
                std::placeholders::_3));
  dispatcher_->RegisterPbHandle(
      proto::CLOSE_CONNECTION_REQUEST,
      std::bind(&ProxyInstance::HandleCloseConnRequest, this,
                std::placeholders::_1, std::placeholders::_2,
                std::placeholders::_3));
  dispatcher_->RegisterPbHandle(
      proto::PAUSE_SEND_REQUEST,
      std::bind(&ProxyInstance::HandlePauseSendRequest, this,
                std::placeholders::_1, std::placeholders::_2,
                std::placeholders::_3));
  dispatcher_->RegisterPbHandle(
      proto::RESUME_SEND_REQUEST,
      std::bind(&ProxyInstance::HandleResumeSendRequest, this,
                std::placeholders::_1, std::placeholders::_2,
                std::placeholders::_3));
  dispatcher_->RegisterPbHandle(
      proto::PING,
      std::bind(&ProxyInstance::HandleHeartbeat, this, std::placeholders::_1,
                std::placeholders::_2, std::placeholders::_3));
  dispatcher_->RegisterMsgHandle(
      DATA_REQUEST, std::bind(&ProxyInstance::HandleDataRequest, this,
                              std::placeholders::_1, std::placeholders::_2));
  proxy_conn_->setHighWaterMarkCallback(
      std::bind(&ProxyInstance::OnHighWaterMark, this, true,
                std::placeholders::_1, std::placeholders::_2),
      10 * MB_SIZE);
  // 启动定时器,10s之内没有链接就断开
  check_listen_timer_ =
      loop_->runAfter(10.0, std::bind(&ProxyInstance::CheckListen, this));
  // 启动心跳定时器
  heartbeat_timer_ =
      loop_->runEvery(10.0, std::bind(&ProxyInstance::SendHeartBeat, this));
}

void ProxyInstance::Stop(StopCb cb) {
  // 停止accept新连接
  // 等到所有连接都close
  stop_cb_ = cb;
  proxy_client_connect_ = false;
  // 停止心跳定时器
  loop_->cancel(check_listen_timer_);
  loop_->cancel(heartbeat_timer_);
  acceptor_.reset();
  dispatcher_.reset();  // TODO(ke.zhang) 这里是否有内存问题?
  for (auto &conn : conn_map_) {
    conn.second.conn->forceClose();
  }
  CheckStop();
}

void ProxyInstance::HandleListenRequest(const muduo::net::TcpConnectionPtr,
                                        ProxyMessagePtr request_head,
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
    dispatcher_->SendPbResponse(proxy_conn_, request_head,
                                listen_response_msg_);
    return;
  }
  uint16_t listen_port =
      static_cast<uint16_t>(message->body().listen_request().listen_port());
  listen_addr_ =
      muduo::net::InetAddress(proxy_conn_->localAddress().toIp(), listen_port);
  // listen_addr_ = muduo::net::InetAddress("0.0.0.0", listen_port);
  auto listen_result = StartListen();
  if (listen_result.first) {
    response_body->mutable_rc()->set_retcode(0);
    response_body->mutable_rc()->set_error_message("success");
  } else {
    LOG_ERROR << "listen failed, port:" << listen_port
              << " error:" << listen_result.second;
    acceptor_.reset();
    response_body->mutable_rc()->set_retcode(-1);
    response_body->mutable_rc()->set_error_message(listen_result.second);
  }
  dispatcher_->SendPbResponse(proxy_conn_, request_head, listen_response_msg_);
  return;
}

std::pair<bool, std::string> ProxyInstance::StartListen() {
  loop_->assertInLoopThread();
  LOG_INFO << "proxy listen client_addr:"
           << proxy_conn_->peerAddress().toIpPort()
           << " listen_addr:" << listen_addr_.toIpPort();
  acceptor_.reset(new Acceptor(loop_, listen_addr_, false));
  acceptor_->setNewConnectionCallback(std::bind(&ProxyInstance::OnNewConnection,
                                                this, std::placeholders::_1,
                                                std::placeholders::_2));
  thread_pool_.reset(
      new muduo::net::EventLoopThreadPool(loop_, "proxy_server"));
  thread_pool_->start(nullptr);
  return acceptor_->listen();
}

void ProxyInstance::OnNewConnection(int sockfd,
                                    const muduo::net::InetAddress &peer_addr) {
  loop_->assertInLoopThread();
  LOG_INFO << "recv client conn, peer addr:" << peer_addr.toIpPort();
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
  conn->setHighWaterMarkCallback(
      std::bind(&ProxyInstance::OnHighWaterMark, this, false,
                std::placeholders::_1, std::placeholders::_2),
      2 * MB_SIZE);
  io_loop->runInLoop(
      std::bind(&muduo::net::TcpConnection::connectEstablished, conn));
}

void ProxyInstance::OnClientConnection(
    const muduo::net::TcpConnectionPtr &conn) {
  loop_->runInLoop([=] {
    if (conn->getContext().empty()) {
      // 连接建立调用
      uint64_t conn_id = GetConnId();
      conn->setContext(conn_id);
      conn_map_.insert(std::make_pair(conn_id, Connection(conn)));
      if (!proxy_client_connect_) {
        LOG_WARN << "proxy disconnect, OnclientConnection, give up";
        conn->forceClose();
        return;
      }
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
        for (uint i = 0;
             i < sizeof(address->sin6_addr.s6_addr) / sizeof(uint8_t); ++i) {
          *(new_connection_request->add_ip_v6()) =
              address->sin6_addr.s6_addr[i];
        }
        new_connection_request->set_port(ntohs(address->sin6_port));
      }
      dispatcher_->SendPbRequest(
          proxy_conn_, message,
          std::bind(&ProxyInstance::EntryAddConnection, this_ptr(),
                    std::placeholders::_1, conn),
          std::bind(&ProxyInstance::AddConnectionTimeout, this_ptr(), conn));
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
    if (!proxy_client_connect_) {
      LOG_WARN << "proxy disconnect, OnClientMessage, give up";
      return;
    }
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    assert(conn_id);
    assert(conn_map_.count(conn_id));
    Connection &client_conn = conn_map_[conn_id];
    if (client_conn.proxy_accept) {
      DataRequestBody data_request;
      data_request.length = buffer->readableBytes();
      data_request.conn_key = conn_id;
      data_request.data = std::string(buffer->peek(), buffer->readableBytes());
      buffer->retrieve(buffer->readableBytes());
      ProxyMessage request_head;
      request_head.message_type = DATA_REQUEST;
      request_head.length = data_request.Size();
      request_head.body = &data_request;
      dispatcher_->SendRequest(
          proxy_conn_, &request_head,
          std::bind(&ProxyInstance::EntryData, this_ptr(),
                    std::placeholders::_1, std::placeholders::_2, conn),
          nullptr);
      request_head.body = nullptr;
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
    LOG_INFO << "client conn close, conn_id:" << conn_id
             << " peer_address:" << conn->peerAddress().toIpPort();
    if (!proxy_client_connect_) {
      // 直接删除
      RemoveConnecion(conn_id);
      CheckStop();
      return;
    }
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::CLOSE_CONNECTION_REQUEST,
                GetSourceEntity());
    proto::CloseConnectionRequest *close_connection_request =
        message->mutable_body()->mutable_close_connection_request();
    close_connection_request->set_conn_key(conn_id);
    dispatcher_->SendPbRequest(
        proxy_conn_, message,
        std::bind(&ProxyInstance::EntryCloseConnection, this_ptr(),
                  std::placeholders::_1, conn_id),
        nullptr);
  });
}

void ProxyInstance::EntryCloseConnection(MessagePtr message, uint64_t conn_id) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_RESONSE);
  assert(message->body().has_close_connection_response());
  // const proto::CloseConnectionResponse &response =
  //     message->body().close_connection_response();
  // 忽略返回码
  if (conn_map_.count(conn_id)) {
    RemoveConnecion(conn_id);
  } else {
    LOG_WARN << "conn_id:" << conn_id << " already removed";
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

void ProxyInstance::AddConnectionTimeout(
    const muduo::net::TcpConnectionPtr &client_conn) {
  LOG_ERROR << "proxy client accept connection timeout";
  // 强制关闭客户端连接
  uint64_t conn_id = boost::any_cast<uint64_t>(client_conn->getContext());
  RemoveConnecion(conn_id);
}

void ProxyInstance::EntryData(const muduo::net::TcpConnectionPtr &conn,
                              ProxyMessagePtr response,
                              const muduo::net::TcpConnectionPtr &client_conn) {
  assert(response->message_type == DATA_RESPONSE);
  uint64_t conn_id = boost::any_cast<uint64_t>(client_conn->getContext());
  DataResponseBody *response_body =
      dynamic_cast<DataResponseBody *>(response->body);
  if (response_body->retcode == 0) {
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
                                      ProxyMessagePtr message) {
  // 判断auth
  assert(message->message_type == DATA_REQUEST);
  DataRequestBody *request = dynamic_cast<DataRequestBody *>(message->body);
  uint64_t conn_key = request->conn_key;
  DataResponseBody response_body;
  if (conn_map_.find(conn_key) != conn_map_.end()) {
    (conn_map_[conn_key])
        .conn->send((request->data).c_str(), (request->data).size());
    response_body.retcode = 0;
  } else {
    response_body.retcode = -1;
  }
  ProxyMessage response_head;
  response_head.message_type = DATA_RESPONSE;
  response_head.length = response_body.Size();
  response_head.request_id = message->request_id;
  response_head.body = &response_body;
  // response_body->
  dispatcher_->SendResponse(proxy_conn_, &response_head);
  response_head.body = nullptr;
  return;
}

void ProxyInstance::HandleCloseConnRequest(const muduo::net::TcpConnectionPtr,
                                           ProxyMessagePtr request_head,
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
    RemoveConnecion(conn_key);
    response_body->mutable_rc()->set_retcode(0);
  } else {
    response_body->mutable_rc()->set_retcode(-1);
  }
  dispatcher_->SendPbResponse(proxy_conn_, request_head, close_response);
}

void ProxyInstance::RemoveConnecion(uint64_t conn_id) {
  auto index = conn_map_.find(conn_id);
  if (index == conn_map_.end()) {
    return;
  }
  index->second.conn->getLoop()->queueInLoop(std::bind(
      &muduo::net::TcpConnection::connectDestroyed, index->second.conn));
  conn_map_.erase(index);
}

void ProxyInstance::HandlePauseSendRequest(const muduo::net::TcpConnectionPtr,
                                           ProxyMessagePtr request_head,
                                           MessagePtr message) {
  assert(message->head().message_type() == proto::PAUSE_SEND_REQUEST);
  assert(message->body().has_pause_send_request());
  const proto::PauseSendRequest &request = message->body().pause_send_request();
  uint64_t conn_key = request.conn_key();
  MessagePtr pause_send_response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::PAUSE_SEND_RESPONSE,
               pause_send_response.get());
  proto::PauseSendResponse *response_body =
      pause_send_response->mutable_body()->mutable_pause_send_response();
  StopClientRead(conn_key, true);
  response_body->mutable_rc()->set_retcode(0);
  dispatcher_->SendPbResponse(proxy_conn_, request_head, pause_send_response);
}

void ProxyInstance::HandleResumeSendRequest(const muduo::net::TcpConnectionPtr,
                                            ProxyMessagePtr request_head,
                                            MessagePtr message) {
  assert(message->head().message_type() == proto::RESUME_SEND_REQUEST);
  assert(message->body().has_resume_send_request());
  const proto::ResumeSendRequest &request =
      message->body().resume_send_request();
  uint64_t conn_key = request.conn_key();
  MessagePtr resume_send_response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::RESUME_SEND_RESPONSE,
               resume_send_response.get());
  proto::ResumeSendResponse *response_body =
      resume_send_response->mutable_body()->mutable_resume_send_response();
  ResumeClientRead(conn_key, true);
  response_body->mutable_rc()->set_retcode(0);
  dispatcher_->SendPbResponse(proxy_conn_, request_head, resume_send_response);
}

void ProxyInstance::StopClientRead(uint64_t conn_id, bool server_block) {
  if (conn_id) {
    auto index = conn_map_.find(conn_id);
    if (index == conn_map_.end()) {
      LOG_WARN << "stop conn_id:" << conn_id
               << " read failed, not found connection";
    } else {
      (index->second).conn->stopRead();
      (index->second).server_block = server_block;
      LOG_INFO << "stop conn_id:" << conn_id << " read succ";
    }
    return;
  } else {
    LOG_INFO << "stop all client connection read";
    for (auto index = conn_map_.begin(); index != conn_map_.end(); ++index) {
      StopClientRead(index->first);
    }
  }
}

void ProxyInstance::ResumeClientRead(uint64_t conn_id, bool server_block) {
  if (conn_id) {
    auto index = conn_map_.find(conn_id);
    if (index == conn_map_.end()) {
      LOG_WARN << "resume conn_id:" << conn_id
               << " read failed, not found connection";
    } else {
      if ((index->second).server_block && !server_block) {
        LOG_INFO << "not resume conn_id:" << conn_id << " read, server block";
      } else {
        (index->second).conn->startRead();
        (index->second).server_block = false;
        LOG_INFO << "resume conn_id:" << conn_id << " read succ";
      }
    }
    return;
  } else {
    LOG_INFO << "resume all client connection read";
    for (auto index = conn_map_.begin(); index != conn_map_.end(); ++index) {
      ResumeClientRead(index->first);
    }
  }
}

void ProxyInstance::OnHighWaterMark(bool is_proxy_conn,
                                    const muduo::net::TcpConnectionPtr &conn,
                                    size_t) {
  if (is_proxy_conn) {
    LOG_INFO << "proxy connection high water";
    if (proxy_conn_->outputBuffer()->readableBytes() > 0) {
      StopClientRead();
      proxy_conn_->setWriteCompleteCallback(std::bind(
          &ProxyInstance::OnWriteComplete, this, true, std::placeholders::_1));
    }
  } else {
    // 客户端接收速度慢,通知proxy client
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    LOG_INFO << "client conn_id:" << conn_id << " high water";
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::PAUSE_SEND_REQUEST, GetSourceEntity());
    proto::PauseSendRequest *pause_send_request =
        message->mutable_body()->mutable_pause_send_request();
    pause_send_request->set_conn_key(conn_id);
    dispatcher_->SendPbRequest(
        proxy_conn_, message,
        std::bind(&ProxyInstance::EntryPauseSend, this_ptr(),
                  std::placeholders::_1, conn_id),
        nullptr);
    conn->setWriteCompleteCallback(std::bind(
        &ProxyInstance::OnWriteComplete, this, false, std::placeholders::_1));
  }
}

void ProxyInstance::OnWriteComplete(bool is_proxy_conn,
                                    const muduo::net::TcpConnectionPtr &conn) {
  if (is_proxy_conn) {
    LOG_INFO << "proxy connection write complete";
    ResumeClientRead();
    proxy_conn_->setWriteCompleteCallback(muduo::net::WriteCompleteCallback());
  } else {
    // 通知proxy client可以继续发送
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    LOG_INFO << "client conn_id:" << conn_id << " write complete";
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::RESUME_SEND_REQUEST, GetSourceEntity());
    proto::ResumeSendRequest *resume_send_request =
        message->mutable_body()->mutable_resume_send_request();
    resume_send_request->set_conn_key(conn_id);
    dispatcher_->SendPbRequest(
        proxy_conn_, message,
        std::bind(&ProxyInstance::EntryResumeSend, this_ptr(),
                  std::placeholders::_1, conn_id),
        nullptr);
    conn->setWriteCompleteCallback(muduo::net::WriteCompleteCallback());
  }
}

void ProxyInstance::EntryPauseSend(MessagePtr, uint64_t conn_id) {}

void ProxyInstance::EntryResumeSend(MessagePtr, uint64_t conn_id) {}

void ProxyInstance::CheckListen() {
  if (!acceptor_) {
    LOG_WARN << "not listen request, peer_address:"
             << proxy_conn_->peerAddress().toIpPort();
    proxy_conn_->forceClose();
  }
}

void ProxyInstance::CheckStop() {
  if (conn_map_.empty()) {
    LOG_INFO << "ProxyInstance can stop now";
    if (stop_cb_) {
      stop_cb_();
    }
  }
}

void ProxyInstance::SendHeartBeat() {
  MessagePtr message = std::make_shared<proto::Message>();
  MakeMessage(message.get(), proto::PING, GetSourceEntity());
  proto::Ping *ping_request = message->mutable_body()->mutable_ping();
  ping_request->set_time(time(nullptr));
  LOG_DEBUG << "ping to client, time:" << ping_request->time();
  dispatcher_->SendPbRequest(proxy_conn_, message,
                             std::bind(&ProxyInstance::EntryHeartBeat,
                                       this_ptr(), std::placeholders::_1),
                             nullptr);
}

void ProxyInstance::EntryHeartBeat(MessagePtr message) {
  assert(message->head().message_type() == proto::PONG);
  assert(message->body().has_pong());
  const proto::Pong &response = message->body().pong();
  LOG_DEBUG << "recv pong from client, time:" << response.time();
}

void ProxyInstance::HandleHeartbeat(const muduo::net::TcpConnectionPtr,
                                    ProxyMessagePtr request_head,
                                    MessagePtr message) {
  MessagePtr pong_response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::PONG, pong_response.get());
  proto::Pong *response_body = pong_response->mutable_body()->mutable_pong();
  response_body->mutable_rc()->set_retcode(0);
  response_body->set_time(time(nullptr));
  dispatcher_->SendPbResponse(proxy_conn_, request_head, pong_response);
}