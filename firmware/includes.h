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

#include "esphome/core/log.h"

// Open a TCP connection to (ip, port) with a connect timeout in ms.
// Returns the connected socket fd on success, -1 on failure.
// On failure, if err_out is non-null, receives the errno-style code that
// distinguishes the failure mode (ECONNREFUSED = AVR up but no listener,
// ETIMEDOUT = no response in window, EHOSTUNREACH / ENETUNREACH = wrong
// subnet / device off, EINVAL = ip wasn't a valid IPv4 literal). Caller is
// responsible for ::close()ing the fd on success.
inline int tcp_connect(const char *ip, uint16_t port, int connect_timeout_ms,
                       int *err_out = nullptr) {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) { if (err_out) *err_out = errno; return -1; }

  // Non-blocking connect so we can enforce a real timeout.
  int flags = ::fcntl(sock, F_GETFL, 0);
  ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    if (err_out) *err_out = EINVAL;
    ::close(sock);
    return -1;
  }

  int rc = ::connect(sock, (struct sockaddr *) &addr, sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    if (err_out) *err_out = errno;
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
    if (err_out) *err_out = (rc == 0) ? ETIMEDOUT : errno;
    ::close(sock);
    return -1;
  }

  int err = 0;
  socklen_t errlen = sizeof(err);
  if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
    if (err_out) *err_out = err ? err : errno;
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

// Read for the full timeout window, appending everything received to `out`.
// Unlike tcp_read_until_cr, does not stop at CR — useful for capturing the AVR's
// echo stream where multiple sent commands each produce their own CR line.
inline void tcp_read_all(int sock, std::string &out, int timeout_ms) {
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
    char buf[128];
    ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, n);
    remaining = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
  }
}

// tcp_connect with one automatic retry after 500 ms. Logs each failed attempt
// at WARN through `tag` (typically "avr" / "pjlink") so we get errno context
// in the ESPHome log without every caller re-writing the boilerplate.
// Returns socket fd on success, -1 on final failure.
inline int tcp_connect_retry(const char *tag, const char *ip, uint16_t port,
                             int connect_timeout_ms, int retries,
                             int *err_out = nullptr) {
  int last_err = 0;
  for (int attempt = 0; attempt <= retries; attempt++) {
    int s = tcp_connect(ip, port, connect_timeout_ms, &last_err);
    if (s >= 0) return s;
    ESP_LOGW(tag, "connect fail attempt %d/%d (errno=%d %s)",
             attempt + 1, retries + 1, last_err, ::strerror(last_err));
    if (attempt < retries) ::usleep(500 * 1000);
  }
  if (err_out) *err_out = last_err;
  return -1;
}

// Send N commands in sequence over an already-connected socket, spacing them
// by inter_cmd_ms (Denon docs recommend ~50 ms; some X-series firmware silently
// drops back-to-back writes). After the last send, reads acks for ack_timeout_ms
// and verifies each `expected[i]` substring appears in the response.
// Returns true if every expected token was echoed, false otherwise. Logs each
// missing ack and the collected reply through `tag`.
// Caller owns the socket; this function does not close it.
inline bool tcp_send_and_verify(const char *tag, int sock,
                                const char *const *cmds,
                                const char *const *expected,
                                int count,
                                int inter_cmd_ms = 50,
                                int ack_timeout_ms = 500) {
  for (int i = 0; i < count; i++) {
    if (!tcp_send(sock, cmds[i], std::strlen(cmds[i]))) {
      ESP_LOGW(tag, "send fail on cmd[%d]='%s'", i, cmds[i]);
      return false;
    }
    if (i + 1 < count && inter_cmd_ms > 0) {
      ::usleep(inter_cmd_ms * 1000);
    }
  }
  std::string acks;
  tcp_read_all(sock, acks, ack_timeout_ms);
  bool all_ok = true;
  for (int i = 0; i < count; i++) {
    if (acks.find(expected[i]) == std::string::npos) {
      ESP_LOGW(tag, "no ack for '%s'", expected[i]);
      all_ok = false;
    }
  }
  // Render CRs as '|' for a single readable log line.
  std::string log_acks;
  for (char c : acks) log_acks += (c == '\r' ? '|' : c);
  if (all_ok) ESP_LOGI(tag, "ok (ack: %s)", log_acks.c_str());
  else        ESP_LOGW(tag, "partial ack (got: %s)", log_acks.c_str());
  return all_ok;
}
