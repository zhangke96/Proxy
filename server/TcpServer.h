// Copyright [2020] zhangke

#ifndef SERVER_TCPSERVER_H_
#define SERVER_TCPSERVER_H_

#include <muduo/base/Atomic.h>
#include <muduo/base/noncopyable.h>
#include <muduo/net/Acceptor.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <map>
#include <memory>
#include <set>
#include <string>

class TcpServer : muduo::noncopyable {
 public:
  typedef std::function<void(muduo::net::EventLoop*)> ThreadInitCallback;
  enum Option {
    kNoReusePort,
    kReusePort,
  };

  struct Connection {
    explicit Connection(muduo::net::TcpConnectionPtr conn)
        : conn(conn), server_close(false), client_close(false) {}
    muduo::net::TcpConnectionPtr conn;
    bool server_close;
    bool client_close;
    // friend bool operator<(const Connection& lhs, const Connection& rhs) {
    //   return lhs.conn.get() < rhs.conn.get();
    // }
  };

  TcpServer(muduo::net::EventLoop* loop,
            const muduo::net::InetAddress& listenAddr,
            const std::string& nameArg, Option option = kNoReusePort);
  ~TcpServer();  // force out-line dtor, for std::unique_ptr members.

  const std::string& ipPort() const { return ipPort_; }
  const std::string& name() const { return name_; }
  muduo::net::EventLoop* getLoop() const { return loop_; }

  /// Set the number of threads for handling input.
  ///
  /// Always accepts new connection in loop's thread.
  /// Must be called before @c start
  /// @param numThreads
  /// - 0 means all I/O in loop's thread, no thread will created.
  ///   this is the default value.
  /// - 1 means all I/O in another thread.
  /// - N means a thread pool with N threads, new connections
  ///   are assigned on a round-robin basis.
  void setThreadNum(int numThreads);
  void setThreadInitCallback(const ThreadInitCallback& cb) {
    threadInitCallback_ = cb;
  }
  /// valid after calling start()
  std::shared_ptr<muduo::net::EventLoopThreadPool> threadPool() {
    return threadPool_;
  }

  /// Starts the server if it's not listenning.
  ///
  /// It's harmless to call it multiple times./*  */
  /// Thread safe.
  void start();

  /// Set connection callback.
  /// Not thread safe.
  void setConnectionCallback(const muduo::net::ConnectionCallback& cb) {
    connectionCallback_ = cb;
  }

  void setConnectionCloseCallback(const muduo::net::CloseCallback& cb) {
    closeCallback_ = cb;
  }

  /// Set message callback.
  /// Not thread safe.
  void setMessageCallback(const muduo::net::MessageCallback& cb) {
    messageCallback_ = cb;
  }

  /// Set write complete callback.
  /// Not thread safe.
  void setWriteCompleteCallback(const muduo::net::WriteCompleteCallback& cb) {
    writeCompleteCallback_ = cb;
  }

  void shutdownConn(const muduo::net::TcpConnectionPtr& conn);

  void onClientClose(const muduo::net::TcpConnectionPtr& conn);

 private:
  /// Not thread safe, but in loop
  void newConnection(int sockfd, const muduo::net::InetAddress& peerAddr);
  /// Thread safe.
  void removeConnection(const muduo::net::TcpConnectionPtr& conn);
  /// Not thread safe, but in loop
  void removeConnectionInLoop(const muduo::net::TcpConnectionPtr& conn);

  muduo::net::EventLoop* loop_;  // the acceptor loop
  const std::string ipPort_;
  const std::string name_;
  std::unique_ptr<muduo::net::Acceptor> acceptor_;  // avoid revealing Acceptor
  std::shared_ptr<muduo::net::EventLoopThreadPool> threadPool_;
  muduo::net::ConnectionCallback connectionCallback_;
  muduo::net::CloseCallback closeCallback_;
  muduo::net::MessageCallback messageCallback_;
  muduo::net::WriteCompleteCallback writeCompleteCallback_;
  ThreadInitCallback threadInitCallback_;
  muduo::AtomicInt32 started_;
  // always in loop thread
  int nextConnId_;
  // std::set<Connection> connections_;
  std::map<muduo::net::TcpConnection*, Connection> connections_;
};

#endif  // SERVER_TCPSERVER_H_
