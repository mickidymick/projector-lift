#pragma once

#include <string>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>

// Open a TCP connection to (ip, port) with a connect timeout in ms.
// Returns the connected socket fd on success, -1 on failure.
// Caller is responsible for ::close()ing the fd.
inline int tcp_connect(const char *ip, uint16_t port, int connect_timeout_ms) {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return -1;

  // Non-blocking connect so we can enforce a real timeout.
  int flags = ::fcntl(sock, F_GETFL, 0);
  ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    ::close(sock);
    return -1;
  }

  int rc = ::connect(sock, (struct sockaddr *) &addr, sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    ::close(sock);
    return -1;
  }

  fd_set wset;
  FD_ZERO(&wset);
  FD_SET(sock, &wset);
  struct timeval tv;
  tv.tv_sec  = connect_timeout_ms / 1000;
  tv.tv_usec = (connect_timeout_ms % 1000) * 1000;
  rc = ::select(sock + 1, nullptr, &wset, nullptr, &tv);
  if (rc <= 0 || !FD_ISSET(sock, &wset)) {
    ::close(sock);
    return -1;
  }

  int err = 0;
  socklen_t errlen = sizeof(err);
  if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
    ::close(sock);
    return -1;
  }

  // Back to blocking for clean send/recv with our own select-based read timeout.
  ::fcntl(sock, F_SETFL, flags);
  return sock;
}

// Send a complete buffer, blocking until done or error.
inline bool tcp_send(int sock, const char *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(sock, buf + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += n;
  }
  return true;
}

// Read into `out` until we see '\r' or timeout. Returns true if any bytes read.
inline bool tcp_read_until_cr(int sock, std::string &out, int timeout_ms) {
  int remaining = timeout_ms;
  while (remaining > 0) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    struct timeval tv;
    tv.tv_sec  = remaining / 1000;
    tv.tv_usec = (remaining % 1000) * 1000;
    int rc = ::select(sock + 1, &rset, nullptr, nullptr, &tv);
    if (rc <= 0) break;
    if (FD_ISSET(sock, &rset)) {
      char buf[64];
      ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
      if (n <= 0) break;
      out.append(buf, n);
      if (out.find('\r') != std::string::npos) return true;
    }
    remaining = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
  }
  return !out.empty();
}
