// Copyright [2020] zhangke

#include "client/proxy_client.h"
#include <muduo/base/Logging.h>
#include <boost/any.hpp>
#include <utility>
#include "common/message.pb.h"
#include "common/message_util.h"

int ProxyClient::Start() {
  muduo::Logger::setLogLevel(muduo::Logger::DEBUG);
  event_loop_thread_.reset(new muduo::net::EventLoopThread);
  loop_ = event_loop_thread_->startLoop();
  StartProxyService();
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
    proxy_client_.reset(
        new muduo::net::TcpClient(loop_, server_address_, "proxy_connection"));
    proxy_client_->setConnectionCallback(std::bind(
        &ProxyClient::OnProxyConnection, this_ptr(), std::placeholders::_1));
    proxy_client_->setMessageCallback(
        std::bind(&ProxyClient::OnMessage, this_ptr(), std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));
    proxy_client_->connect();
  });
}

void ProxyClient::OnProxyConnection(const muduo::net::TcpConnectionPtr &) {
  LOG_INFO << "proxy connection established";
  // 注册pb handle
  RegisterHandle(proto::NEW_CONNECTION_REQUEST,
                 std::bind(&ProxyClient::OnNewConnection, this_ptr(),
                           std::placeholders::_1, std::placeholders::_2));
  RegisterHandle(proto::DATA_REQUEST,
                 std::bind(&ProxyClient::OnNewData, this_ptr(),
                           std::placeholders::_1, std::placeholders::_2));
  RegisterHandle(proto::CLOSE_CONNECTION_REQUEST,
                 std::bind(&ProxyClient::OnCloseConnection, this_ptr(),
                           std::placeholders::_1, std::placeholders::_2));
  // 发送listen request
  MessagePtr message(std::make_shared<proto::Message>());
  MakeMessage(message.get(), proto::LISTEN_REQUEST, GetSourceEntity());
  proto::ListenRequest *listen_request =
      message->mutable_body()->mutable_listen_request();
  uint32_t ip =
      ntohl(((sockaddr_in *)local_address_.getSockAddr())->sin_addr.s_addr);
  listen_request->set_self_ipv4(ip);
  listen_request->set_self_port(local_address_.toPort());
  listen_request->set_listen_port(listen_port_);
  SendRequest(proxy_client_->connection(), message,
              std::bind(&ProxyClient::HandleListenResponse, this_ptr(),
                        std::placeholders::_1));
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
                                  MessagePtr message) {
  // 连接到本地
  assert(message->head().message_type() == proto::NEW_CONNECTION_REQUEST);
  assert(message->body().has_new_connection_request());
  const proto::NewConnectionRequest &new_connection_request =
      message->body().new_connection_request();
  muduo::net::InetAddress remote_address(new_connection_request.ip_v4(),
                                         new_connection_request.port());
  uint64_t conn_key = new_connection_request.conn_key();
  std::unique_ptr<TcpClient> tcp_client(new TcpClient(loop_, local_address_));
  tcp_client->SetConnectionCallback(std::bind(&ProxyClient::OnClientConnection,
                                              this_ptr(), std::placeholders::_1,
                                              conn_key));
  tcp_client->SetMessageCallback(std::bind(
      &ProxyClient::OnClientMessage, this_ptr(), std::placeholders::_1,
      std::placeholders::_2, std::placeholders::_3));
  tcp_client->SetCloseCallback(std::bind(&ProxyClient::OnClientClose,
                                         this_ptr(), std::placeholders::_1,
                                         conn_key));
  tcp_client->Connect();
  ProxyConnection proxy_connection;
  proxy_connection.conn_key = conn_key;
  proxy_connection.client_conn = std::move(tcp_client);
  proxy_connection.state = ProxyConnState::CONNECTING;
  proxy_connection.client_open = true;
  proxy_connection.server_open = false;
  proxy_connection.connect_request = message;
  assert(clients_.find(conn_key) == clients_.end());
  clients_[conn_key] = std::move(proxy_connection);
}

void ProxyClient::OnClientConnection(const muduo::net::TcpConnectionPtr &conn,
                                     uint64_t conn_key) {
  // 添加到记录中
  // 如果找不到是destroy conn
  if (clients_.find(conn_key) == clients_.end()) {
    return;
  }
  ProxyConnection &proxy_connection = clients_[conn_key];
  if (proxy_connection.state == ProxyConnState::CONNECTING) {
    proxy_connection.client_conn->Connection()->setContext(conn_key);
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
    SendResponse(proxy_client_->connection(), response_message);
    for (auto &data : proxy_connection.pending_data) {
      proxy_connection.client_conn->Connection()->send(data.c_str(),
                                                       data.size());
    }
    if (proxy_connection.client_open == false) {
      proxy_connection.client_conn->Disconnect();
    }
  }
}

void ProxyClient::OnClientMessage(const muduo::net::TcpConnectionPtr &conn,
                                  muduo::net::Buffer *buffer,
                                  muduo::Timestamp) {
  LOG_DEBUG << "receive from server";
  uint64_t conn_key = boost::any_cast<uint64_t>(conn->getContext());
  MessagePtr request_message = std::make_shared<proto::Message>();
  MakeMessage(request_message.get(), proto::DATA_REQUEST, GetSourceEntity(), "",
              session_key_);
  proto::DataRequest *data_request =
      request_message->mutable_body()->mutable_data_request();
  data_request->set_conn_key(conn_key);
  data_request->add_data(std::string(buffer->peek(), buffer->readableBytes()));
  buffer->retrieveAll();
  SendRequest(proxy_client_->connection(), request_message,
              std::bind(&ProxyClient::HandleDataResponse, this_ptr(),
                        std::placeholders::_1));
}

void ProxyClient::HandleDataResponse(MessagePtr message) {}

void ProxyClient::OnNewData(const muduo::net::TcpConnectionPtr &conn,
                            MessagePtr message) {
  assert(message->head().message_type() == proto::DATA_REQUEST);
  assert(message->body().has_data_request());
  const proto::DataRequest &data_request = message->body().data_request();
  uint64_t conn_key = data_request.conn_key();
  MessagePtr response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::DATA_RESPONSE, response.get());
  proto::DataResponse *data_response =
      response->mutable_body()->mutable_data_response();
  if (clients_.find(conn_key) != clients_.end()) {
    auto &client_connection = clients_[conn_key];
    if (client_connection.state == ProxyConnState::CONNECTING) {
      for (int i = 0; i < data_request.data_size(); ++i) {
        client_connection.pending_data.push_back(data_request.data(i));
      }
    } else {
      for (int i = 0; i < data_request.data_size(); ++i) {
        const std::string &data = data_request.data(i);
        client_connection.client_conn->Connection()->send(data.c_str(),
                                                          data.size());
      }
    }
    data_response->mutable_rc()->set_retcode(0);
  } else {
    data_response->mutable_rc()->set_retcode(-1);
  }
  SendResponse(proxy_client_->connection(), response);
}

void ProxyClient::OnCloseConnection(const muduo::net::TcpConnectionPtr &conn,
                                    MessagePtr message) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_REQUEST);
  assert(message->body().has_close_connection_request());
  const proto::CloseConnectionRequest &close_connection_request =
      message->body().close_connection_request();
  uint64_t conn_key = close_connection_request.conn_key();
  MessagePtr response = std::make_shared<proto::Message>();
  MakeResponse(message.get(), proto::CLOSE_CONNECTION_RESONSE, response.get());
  proto::CloseConnectionResponse *close_connection_response =
      response->mutable_body()->mutable_close_connection_response();
  if (clients_.find(conn_key) != clients_.end()) {
    auto &client_connection = clients_[conn_key];
    client_connection.client_open = false;
    if (client_connection.server_open) {
      client_connection.client_conn->Disconnect();
    } else {
      RemoveConnection(conn_key);
    }
    close_connection_response->mutable_rc()->set_retcode(0);
  } else {
    close_connection_response->mutable_rc()->set_retcode(-1);
  }
  SendResponse(proxy_client_->connection(), response);
}

void ProxyClient::OnClientClose(const muduo::net::TcpConnectionPtr &,
                                uint64_t conn_key) {
  LOG_DEBUG << "server shutdown write";
  assert(clients_.find(conn_key) != clients_.end());
  ProxyConnection &proxy_connection = clients_[conn_key];
  // proxy_connection.server_open = false;
  MessagePtr request_message = std::make_shared<proto::Message>();
  MakeMessage(request_message.get(), proto::CLOSE_CONNECTION_REQUEST,
              GetSourceEntity(), "", session_key_);
  proto::CloseConnectionRequest *close_conn_request =
      request_message->mutable_body()->mutable_close_connection_request();
  close_conn_request->set_conn_key(conn_key);
  SendRequest(proxy_client_->connection(), request_message,
              std::bind(&ProxyClient::HandleCloseResponse, this_ptr(),
                        std::placeholders::_1, conn_key));
}

void ProxyClient::HandleCloseResponse(MessagePtr message, uint64_t conn_key) {
  assert(message->head().message_type() == proto::CLOSE_CONNECTION_RESONSE);
  assert(message->body().has_close_connection_response());
  // 忽略返回码
  assert(clients_.find(conn_key) != clients_.end());
  ProxyConnection &proxy_connection = clients_[conn_key];
  proxy_connection.server_open = false;
  if (proxy_connection.client_open == false) {
    RemoveConnection(conn_key);
  }
}

void ProxyClient::RemoveConnection(uint64_t conn_key) {
  LOG_INFO << "remove connection, conn_key:" << conn_key;
  assert(clients_.count(conn_key));
  ProxyConnection &proxy_connection = clients_[conn_key];
  assert(proxy_connection.client_open == false &&
         proxy_connection.server_open == false);
  proxy_connection.client_conn->DestroyConn();
  clients_.erase(conn_key);
}
