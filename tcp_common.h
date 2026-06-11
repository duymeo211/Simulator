// tcp_common.h
#pragma once

#ifdef _WIN32
  #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  typedef SOCKET socket_t;
  #define close_socket closesocket
  #define socket_invalid INVALID_SOCKET
  #define socket_error SOCKET_ERROR
#else
  #include <errno.h>
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netdb.h>
  typedef int socket_t;
  #define close_socket close
  #define socket_invalid (-1)
  #define socket_error (-1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline int net_init(void) {
#ifdef _WIN32
  WSADATA wsa;
  return (WSAStartup(MAKEWORD(2,2), &wsa) == 0) ? 0 : -1;
#else
  return 0;
#endif
}

static inline void net_cleanup(void) {
#ifdef _WIN32
  WSACleanup();
#endif
}

static inline void print_last_error(const char* msg) {
#ifdef _WIN32
  int e = WSAGetLastError();
  fprintf(stderr, "%s (WSA err=%d)\n", msg, e);
#else
  fprintf(stderr, "%s (errno=%d: %s)\n", msg, errno, strerror(errno));
#endif
}

// Gửi đủ len bytes (TCP có thể gửi từng phần)
static inline int send_all(socket_t s, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = (int)send(s, (const char*)data + sent, (int)(len - sent), 0);
    if (n == socket_error) return -1;
    if (n == 0) return -2; // peer closed
    sent += (size_t)n;
  }
  return 0;
}

// Nhận dữ liệu với timeout (ms). Trả về:
//  >0: số bytes nhận được
//   0: peer đóng kết nối
//  -1: lỗi
//  -2: timeout
static inline int recv_with_timeout(socket_t s, uint8_t* buf, int bufsize, int timeout_ms) {
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(s, &rfds);

  struct timeval tv;
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int r = select((int)(s + 1), &rfds, NULL, NULL, &tv);
  if (r == 0) return -2;          // timeout
  if (r == socket_error) return -1;

  int n = (int)recv(s, (char*)buf, bufsize, 0);
  if (n == socket_error) return -1;
  return n; // 0 means closed
}