// Copyright [2020] zhangke

#include "client/proxy_client.h"

#include <muduo/base/Logging.h>

#include <boost/any.hpp>
#include <utility>

#include "common/message.pb.h"
#include "common/message_util.h"

int ProxyClient::Start() {
  std::call_once(start_flag_, &ProxyClient::StartProxyService, this);
  {
    muduo::MutexLockGuard lock(mutex_);
    while (!start_finish_) {
      cond_.wait();
    }
  }
  return start_retcode_;
}

void ProxyClient::StartProxyService() {
  // 建立连接
  loop_->runInLoop([=] {
    dispatcher_->Init();
    proxy_client_.reset(
        new muduo::net::TcpClient(loop_, server_address_, "proxy_connection"));
    // 连接到proxy server成功
    proxy_client_->setConnectionCallback(std::bind(
        &ProxyClient::OnProxyConnection, this_ptr(), std::placeholders::_1));
    // proxy server conn 收到消息
    proxy_client_->setMessageCallback(
        std::bind(&ProxyClient::OnMessage, this_ptr(), std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));
    proxy_client_->enableRetry();
    proxy_client_->connect();
  });
}

void ProxyClient::OnProxyConnection(const muduo::net::TcpConnectionPtr &conn) {
  if (conn->connected()) {
    if (!clients_.empty()) {
      LOG_INFO << "clients not empty, connect to server later, client count:"
               << clients_.size();
      loop_->runAfter(1.0,
                      std::bind(&ProxyClient::OnProxyConnection, this, conn));
      return;
    }
    LOG_INFO << "proxy connection established";
    // 注册高水位回调
    proxy_client_->connection()->setHighWaterMarkCallback(
        std::bind(&ProxyClient::OnHighWaterMark, this, true,
                  std::placeholders::_1, std::placeholders::_2),
        10 * MB_SIZE);
    // 开始心跳
    heartbeat_timer_ =
        loop_->runEvery(10.0, std::bind(&ProxyClient::SendHeartBeat, this));
    if (first_connect_) {
      first_connect_ = false;
      // 注册pb handle
      dispatcher_->RegisterPbHandle(
          proto::NEW_CONNECTION_REQUEST,
          std::bind(&ProxyClient::OnNewConnection, this_ptr(),
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));
      dispatcher_->RegisterPbHandle(
          proto::CLOSE_CONNECTION_REQUEST,
          std::bind(&ProxyClient::OnCloseConnection, this_ptr(),
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));
      dispatcher_->RegisterPbHandle(
          proto::PAUSE_SEND_REQUEST,
          std::bind(&ProxyClient::HandlePauseSendRequest, this_ptr(),
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));
      dispatcher_->RegisterPbHandle(
          proto::RESUME_SEND_REQUEST,
          std::bind(&ProxyClient::HandleResumeSendRequest, this_ptr(),
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));
      dispatcher_->RegisterPbHandle(
          proto::PING, std::bind(&ProxyClient::HandleHeartbeat, this_ptr(),
                                 std::placeholders::_1, std::placeholders::_2,
                                 std::placeholders::_3));
      dispatcher_->RegisterMsgHandle(
          DATA_REQUEST,
          std::bind(&ProxyClient::OnNewData, this_ptr(), std::placeholders::_1,
                    std::placeholders::_2));
    }
    // 发送listen request
    // 重新连接之后需要发送
    MessagePtr message(std::make_shared<proto::Message>());
    MakeMessage(message.get(), proto::LISTEN_REQUEST, GetSourceEntity());
    proto::ListenRequest *listen_request =
        message->mutable_body()->mutable_listen_request();
    uint32_t ip =
        ntohl(((sockaddr_in *)local_address_.getSockAddr())->sin_addr.s_addr);
    listen_request->set_self_ipv4(ip);
    listen_request->set_self_port(local_address_.port());
    listen_request->set_listen_port(listen_port_);
    dispatcher_->SendPbRequest(proxy_client_->connection(), message,
                               std::bind(&ProxyClient::HandleListenResponse,
                                         this_ptr(), std::placeholders::_1),
                               nullptr);
  } else {
    LOG_WARN << "proxy connection disconnected, exist conn count:"
             << clients_.size();
    loop_->cancel(heartbeat_timer_);
    std::vector<uint64_t> exist_connections;
    exist_connections.reserve(clients_.size());
    for (const auto &connection : clients_) {
      exist_connections.push_back(connection.first);
    }
    for (uint64_t conn_key : exist_connections) {
      ClientClose(conn_key);
    }
  }
}

void ProxyClient::HandleListenResponse(MessagePtr response) {
  assert(response->head().message_type() == proto::LISTEN_RESPONSE);
  assert(response->body().has_listen_response());
  const proto::ListenResponse &listen_response =
      response->body().listen_response();
  if (listen_response.rc().retcode() == 0) {
    session_key_ = listen_response.session_key();
  } else {
    start_retcode_ = -1;
  }
  {
    muduo::MutexLockGuard lock(mutex_);
    start_finish_ = true;
    cond_.notify();
  }
}

void ProxyClient::OnNewConnection(const muduo::net::TcpConnectionPtr &conn,
                                  ProxyMessagePtr request_head,
                                  MessagePtr message) {
  // 连接到本地
  assert(message->head().message_type() == proto::NEW_CONNECTION_REQUEST);
  assert(message->body().has_new_connection_request());
  const proto::NewConnectionRequest &new_connection_request =
      message->body().new_connection_request();
  muduo::net::InetAddress remote_address(new_connection_request.ip_v4(),
                                         new_connection_request.port());
  uint64_t conn_key = new_connection_request.conn_key();
  LOG_INFO << "proxy connect, conn_key:" << conn_key
           << " origin client addr:" << remote_address.toIpPort()
           << ", connect to:" << local_address_.toIpPort();
  std::unique_ptr<TcpClient> tcp_client(new TcpClient(loop_, local_address_));
  tcp_client->SetConnectionCallback(std::bind(&ProxyClient::OnClientConnection,
                                              this_ptr(), std::placeholders::_1,
                                              conn_key, request_head));
  tcp_client->SetMessageCallback(std::bind(
      &ProxyClient::OnClientMessage, this_ptr(), std::placeholders::_1,
      std::placeholders::_2, std::placeholders::_3));
  tcp_client->Connect();
  ProxyConnection proxy_connection;
  proxy_connection.conn_key = conn_key;
  proxy_connection.client_conn = std::move(tcp_client);
  proxy_connection.state = ProxyConnState::CONNECTING;
  proxy_connection.server_open = false;
  proxy_connection.client_open = true;
  proxy_connection.connect_request = message;
  proxy_connection.client_block = false;
  assert(clients_.find(conn_key) == clients_.end());
  clients_[conn_key] = std::move(proxy_connection);
}

void ProxyClient::OnClientConnection(const muduo::net::TcpConnectionPtr &conn,
                                     uint64_t conn_key,
                                     ProxyMessagePtr request_head) {
  // 添加到记录中
  // 如果找不到是destroy conn
  if (clients_.find(conn_key) == clients_.end()) {
    return;
  }
  ProxyConnection &proxy_connection = clients_[conn_key];
  if (proxy_connection.state == ProxyConnState::CONNECTING) {
    LOG_INFO << "conn to server succ, conn_key:" << conn_key
             << " proxy conn addr:" << conn->localAddress().toIpPort();
    proxy_connection.client_conn->Connection()->setContext(conn_key);
    // 注册高水位回调
    conn->setHighWaterMarkCallback(
        std::bind(&ProxyClient::OnHighWaterMark, this, false,
                  std::placeholders::_1, std::placeholders::_2),
        2 * MB_SIZE);
    // 连接server成功
    proxy_connection.state = ProxyConnState::CONNECTED;
    proxy_connection.server_open = true;
    // 响应给proxy server
    MessagePtr response_message = std::make_shared<proto::Message>();
    MakeResponse(proxy_connection.connect_request.get(),
                 proto::NEW_CONNECTION_RESPONSE, response_message.get());
    proto::NewConnectionResponse *response =
        response_message->mutable_body()->mutable_new_connection_response();
    response->mutable_rc()->set_retcode(0);
    dispatcher_->SendPbResponse(proxy_client_->connection(), request_head,
                                response_message);
    for (auto &data : proxy_connection.pending_data) {
      proxy_connection.client_conn->Connection()->send(data.c_str(),
                                                       data.size());
    }
  } else {
    LOG_INFO << "conn disconnect, conn_key:" << conn_key;
    assert(conn->disconnected());
    loop_->queueInLoop(
        std::bind(&ProxyClient::OnClientClose, this, conn, conn_key));
  }
}

void ProxyClient::OnClientMessage(const muduo::net::TcpConnectionPtr &conn,
                                  muduo::net::Buffer *buffer,
                                  muduo::Timestamp) {
  uint64_t conn_key = boost::any_cast<uint64_t>(conn->getContext());
  DataRequestBody data_request;
  data_request.length = buffer->readableBytes();
  data_request.conn_key = conn_key;
  data_request.data = std::string(buffer->peek(), buffer->readableBytes());
  buffer->retrieve(buffer->readableBytes());
  ProxyMessage request_head;
  request_head.message_type = DATA_REQUEST;
  request_head.length = data_request.Size();
  request_head.body = &data_request;
  LOG_TRACE << "receive from server, conn_key:" << conn_key
            << " data_length:" << data_request.data.size();
  dispatcher_->SendRequest(
      proxy_client_->connection(), &request_head,
      std::bind(&ProxyClient::HandleDataResponse, this_ptr(),
                std::placeholders::_1, std::placeholders::_2),
      nullptr);
  request_head.body = nullptr;
}

void ProxyClient::HandleDataResponse(const muduo::net::TcpConnectionPtr &conn,
                                     ProxyMessagePtr response) {}

void ProxyClient::OnNewData(const muduo::net::TcpConnectionPtr &conn,
                            ProxyMessagePtr message) {
  assert(message->message_type == DATA_REQUEST);
  DataRequestBody *request = dynamic_cast<DataRequestBody *>(message->body);
  uint64_t conn_key = request->conn_key;
  DataResponseBody response_body;
  if (clients_.find(conn_key) != clients_.end()) {
    LOG_TRACE << "receive from client, conn_key:" << conn_key
              << " data_length:" << request->data.size();
    auto &client_connection = clients_[conn_key];
    if (client_connection.state == ProxyConnState::CONNECTING) {
      client_connection.pending_data.push_back(request->data);
    } else {
      client_connection.client_conn->Connection()->send(request->data.c_str(),
                                                        request->data.size());
    }
    response_body.retcode = 0;
  } else {
    response_body.retcode = -1;
  }
  ProxyMessage response_head;
  response_head.message_type = DATA_RESPONSE;
  response_head.length = response_body.Size();
  response_head.request_id = message->request_id;
  response_head.body = &response_body;
  dispatcher_->SendResponse(conn, &response_head);
  response_head.body = nullptr;
}

void ProxyClient::OnCloseConnection(const muduo::net::TcpConnectionPtr &conn,
                                    ProxyMessagePtr request_head,
                                    MessagePtr message) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_REQUEST);
  assert(message->body().has_close_connection_request());
  const proto::CloseConnectionRequest &close_connection_request =
      message->body().close_connection_request();
  uint64_t conn_key = close_connection_request.conn_key();
  LOG_INFO << "client close conn, conn_key:" << conn_key;
  MessagePtr response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::CLOSE_CONNECTION_RESONSE, response.get());
  proto::CloseConnectionResponse *close_connection_response =
      response->mutable_body()->mutable_close_connection_response();
  if (clients_.find(conn_key) != clients_.end()) {
    // 不支持半连接
    ClientClose(conn_key);
    close_connection_response->mutable_rc()->set_retcode(0);
  } else {
    close_connection_response->mutable_rc()->set_retcode(-1);
  }
  dispatcher_->SendPbResponse(proxy_client_->connection(), request_head,
                              response);
}

void ProxyClient::OnClientClose(const muduo::net::TcpConnectionPtr &,
                                uint64_t conn_key) {
  LOG_INFO << "server shutdown write, conn_key:" << conn_key;
  assert(clients_.find(conn_key) != clients_.end());
  ProxyConnection &proxy_connection = clients_[conn_key];
  proxy_connection.server_open = false;
  LOG_DEBUG << "address: " << &proxy_connection << " conn_key:" << conn_key;
  if (proxy_connection.client_open == true) {
    MessagePtr request_message = std::make_shared<proto::Message>();
    MakeMessage(request_message.get(), proto::CLOSE_CONNECTION_REQUEST,
                GetSourceEntity(), "", session_key_);
    proto::CloseConnectionRequest *close_conn_request =
        request_message->mutable_body()->mutable_close_connection_request();
    close_conn_request->set_conn_key(conn_key);
    dispatcher_->SendPbRequest(
        proxy_client_->connection(), request_message,
        std::bind(&ProxyClient::HandleCloseResponse, this_ptr(),
                  std::placeholders::_1, conn_key),
        nullptr);
  } else {
    LOG_DEBUG << "client already close, conn_key:" << conn_key;
    RemoveConnection(conn_key, false);
  }
}

void ProxyClient::HandleCloseResponse(MessagePtr message, uint64_t conn_key) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_RESONSE);
  assert(message->body().has_close_connection_response());
  // 忽略返回码
  if (clients_.find(conn_key) != clients_.end()) {
    ProxyConnection &proxy_connection = clients_[conn_key];
    proxy_connection.server_open = false;
    RemoveConnection(conn_key);
  } else {
    LOG_WARN << "conn_id:" << conn_key << " already removed";
  }
}

void ProxyClient::ClientClose(uint64_t conn_key) {
  auto &client_connection = clients_[conn_key];
  LOG_DEBUG << "address: " << &client_connection << " conn_key:" << conn_key;
  if (client_connection.client_open == false) {
    LOG_DEBUG << "connection already call close, conn_key:" << conn_key;
    return;
  }
  client_connection.client_open = false;
  if (client_connection.state != ProxyConnState::CONNECTED) {
    LOG_DEBUG << "connection to server not accepted, conn_id:" << conn_key;
    // 到server连接还没建立成功
    client_connection.client_conn->Stop();
    // 为了防止TcpClient.Connector析构时channel没有reset
    // 这里应该获取TcpClient对应的loop
    loop_->runAfter(
        1.0, std::bind(&ProxyClient::RemoveConnection, this, conn_key, false));
  } else if (client_connection.server_open == true) {
    client_connection.client_conn->DestroyConn();
  }
}

void ProxyClient::RemoveConnection(uint64_t conn_key, bool destroy) {
  loop_->queueInLoop([=] {
    LOG_INFO << "remove connection, conn_key:" << conn_key
             << ", destroy(bool): " << destroy;
    if (!clients_.count(conn_key)) {
      LOG_WARN << "conn_key:" << conn_key << " already removed";
      return;
    }
    // 如果到server连接还没建立成功，不能调用DestroyConn
    if (destroy) {
      ProxyConnection &proxy_connection = clients_[conn_key];
      proxy_connection.client_conn->DestroyConn();
    }
    clients_.erase(conn_key);
  });
}

void ProxyClient::HandlePauseSendRequest(const muduo::net::TcpConnectionPtr,
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
  dispatcher_->SendPbResponse(proxy_client_->connection(), request_head,
                              pause_send_response);
}

void ProxyClient::HandleResumeSendRequest(const muduo::net::TcpConnectionPtr,
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
  dispatcher_->SendPbResponse(proxy_client_->connection(), request_head,
                              resume_send_response);
}

void ProxyClient::StopClientRead(uint64_t conn_id, bool client_block) {
  if (conn_id) {
    auto index = clients_.find(conn_id);
    if (index == clients_.end()) {
      LOG_WARN << "stop conn_id:" << conn_id
               << " read failed, not found connection";
    } else {
      if ((index->second).server_open) {
        (index->second).client_conn->Connection()->stopRead();
        if (client_block) {
          // 标识client链接阻塞
          (index->second).client_block = true;
        }
        LOG_INFO << "stop conn_id:" << conn_id << " read succ";
      } else {
        LOG_WARN << "already close conn_id:" << conn_id;
      }
    }
    return;
  } else {
    LOG_INFO << "stop all client connection read";
    for (auto index = clients_.begin(); index != clients_.end(); ++index) {
      StopClientRead(index->first);
    }
  }
}

void ProxyClient::ResumeClientRead(uint64_t conn_id, bool client_block) {
  if (conn_id) {
    auto index = clients_.find(conn_id);
    if (index == clients_.end()) {
      LOG_WARN << "resume conn_id:" << conn_id
               << " read failed, not found connection";
    } else {
      ProxyConnection &conn = index->second;
      if (conn.client_block && !client_block) {
        LOG_INFO << "not resume conn_id:" << conn_id << " read, client block";
      } else if (conn.client_open && conn.server_open) {
        (index->second).client_conn->Connection()->startRead();
        (index->second).client_block = false;
        LOG_INFO << "resume conn_id:" << conn_id << " read succ";
      }
    }
    return;
  } else {
    LOG_INFO << "resume all client connection read";
    for (auto index = clients_.begin(); index != clients_.end(); ++index) {
      ResumeClientRead(index->first);
    }
  }
}

void ProxyClient::OnHighWaterMark(bool is_proxy_conn,
                                  const muduo::net::TcpConnectionPtr &conn,
                                  size_t) {
  if (is_proxy_conn) {
    if (proxy_client_->connection()->outputBuffer()->readableBytes() > 0) {
      StopClientRead();
      proxy_client_->connection()->setWriteCompleteCallback(std::bind(
          &ProxyClient::OnWriteComplete, this, true, std::placeholders::_1));
    }
  } else {
    // 客户端接收速度慢,通知proxy client
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::PAUSE_SEND_REQUEST, GetSourceEntity());
    proto::PauseSendRequest *pause_send_request =
        message->mutable_body()->mutable_pause_send_request();
    pause_send_request->set_conn_key(conn_id);
    dispatcher_->SendPbRequest(
        proxy_client_->connection(), message,
        std::bind(&ProxyClient::EntryPauseSend, this_ptr(),
                  std::placeholders::_1, conn_id),
        nullptr);
    conn->setWriteCompleteCallback(std::bind(
        &ProxyClient::OnWriteComplete, this, false, std::placeholders::_1));
  }
}

void ProxyClient::OnWriteComplete(bool is_proxy_conn,
                                  const muduo::net::TcpConnectionPtr &conn) {
  if (is_proxy_conn) {
    ResumeClientRead();
    proxy_client_->connection()->setWriteCompleteCallback(
        muduo::net::WriteCompleteCallback());
  } else {
    // 通知proxy client可以继续发送
    uint64_t conn_id = boost::any_cast<uint64_t>(conn->getContext());
    MessagePtr message = std::make_shared<proto::Message>();
    MakeMessage(message.get(), proto::RESUME_SEND_REQUEST, GetSourceEntity());
    proto::ResumeSendRequest *resume_send_request =
        message->mutable_body()->mutable_resume_send_request();
    resume_send_request->set_conn_key(conn_id);
    dispatcher_->SendPbRequest(
        proxy_client_->connection(), message,
        std::bind(&ProxyClient::EntryResumeSend, this_ptr(),
                  std::placeholders::_1, conn_id),
        nullptr);
    conn->setWriteCompleteCallback(muduo::net::WriteCompleteCallback());
  }
}

void ProxyClient::EntryPauseSend(MessagePtr, uint64_t conn_id) {}

void ProxyClient::EntryResumeSend(MessagePtr, uint64_t conn_id) {}

void ProxyClient::HandleHeartbeat(const muduo::net::TcpConnectionPtr,
                                  ProxyMessagePtr request_head,
                                  MessagePtr message) {
  MessagePtr pong_response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::PONG, pong_response.get());
  proto::Pong *response_body = pong_response->mutable_body()->mutable_pong();
  response_body->mutable_rc()->set_retcode(0);
  response_body->set_time(time(nullptr));
  dispatcher_->SendPbResponse(proxy_client_->connection(), request_head,
                              pong_response);
}

void ProxyClient::SendHeartBeat() {
  MessagePtr message = std::make_shared<proto::Message>();
  MakeMessage(message.get(), proto::PING, GetSourceEntity());
  proto::Ping *ping_request = message->mutable_body()->mutable_ping();
  ping_request->set_time(time(nullptr));
  LOG_DEBUG << "ping to server, time:" << ping_request->time();
  dispatcher_->SendPbRequest(proxy_client_->connection(), message,
                             std::bind(&ProxyClient::EntryHeartBeat, this_ptr(),
                                       std::placeholders::_1),
                             nullptr);
}

void ProxyClient::EntryHeartBeat(MessagePtr message) {
  assert(message->head().message_type() == proto::PONG);
  assert(message->body().has_pong());
  const proto::Pong &response = message->body().pong();
  LOG_DEBUG << "recv pong from server, time:" << response.time();
}
