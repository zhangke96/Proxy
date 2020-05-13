// Copyright [2020] zhangke

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/types.h>
#include <muduo/net/InetAddress.h>
#include "client/proxy_client.h"

typedef int (*LISTEN)(int sockfd, int backlog);

static const char PROXY_SERVER_ADDR[] = "PROXY_SERVER_ADDR";
static const char PROXY_SERVER_PORT[] = "PROXY_SERVER_PORT";
static const char PROXY_LISTEN_PORT[] = "PROXY_LISTEN_PORT";

static std::shared_ptr<ProxyClient> proxy_client;

int listen(int sockfd, int backlog) {
  char *proxy_server_addr = getenv(PROXY_SERVER_ADDR);
  char *proxy_server_port = getenv(PROXY_SERVER_PORT);
  char *proxy_listen_port = getenv(PROXY_LISTEN_PORT);
  if (!proxy_server_addr || !proxy_server_port || !proxy_listen_port) {
    errno = EDESTADDRREQ;
    return -1;
  }
  // 暂时只支持ipv4
  int ret = 0;
  sockaddr_in server_addr;
  ret = inet_pton(AF_INET, proxy_server_addr, &server_addr);
  if (ret != 1) {
    return -1;
  }
  int port = atoi(proxy_server_port);
  int listen_port = atoi(proxy_listen_port);

  muduo::net::InetAddress server_address(proxy_server_addr, port);
  // 获取本地bind地址
  struct sockaddr_in bind_addr;
  socklen_t addr_len;
  ret = getsockname(sockfd, (struct sockaddr *)&bind_addr, &addr_len);
  if (ret != 0) {
    errno = EDESTADDRREQ;
    return -1;
  }
  muduo::net::InetAddress local_address(bind_addr);
  proxy_client =
      std::make_shared<ProxyClient>(server_address, local_address, listen_port);
  ret = proxy_client->Start();
  if (ret != 0) {
    errno = EDESTADDRREQ;
    return -1;
  }

  void *handle = dlopen("libc.so.6", RTLD_LAZY);
  LISTEN listen_syscal = (LISTEN)dlsym(handle, "listen");
  return listen_syscal(sockfd, backlog);
}
