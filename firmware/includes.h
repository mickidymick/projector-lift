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
// by inter_cmd_ms, then read for ack_timeout_ms and log which expected tokens
// were echoed vs. silent.
//
// Denon X-series Telnet only echoes commands that *change* state — a command
// whose target already matches current state produces no reply, even with a
// long read window. So a missing echo is NOT a failure; it means "already at
// target (or silently accepted)." The only real failure signal we have is
// tcp_send() returning false (socket died mid-write). Callers that want true
// verification should query state after the fact (e.g. `VSMONI ?`).
//
// Returns true unless a TCP send fails. Caller owns the socket.
inline bool tcp_send_and_verify(const char *tag, int sock,
                                const char *const *cmds,
                                const char *const *expected,
                                int count,
                                int inter_cmd_ms = 50,
                                int ack_timeout_ms = 800) {
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

  std::string echoed, silent;
  for (int i = 0; i < count; i++) {
    std::string &bucket = (acks.find(expected[i]) != std::string::npos)
                          ? echoed : silent;
    if (!bucket.empty()) bucket += ",";
    bucket += expected[i];
  }
  // Render CRs as '|' for a single readable log line.
  std::string log_acks;
  for (char c : acks) log_acks += (c == '\r' ? '|' : c);
  ESP_LOGI(tag, "sent=%d echoed=[%s] silent=[%s] reply=%s",
           count, echoed.c_str(), silent.c_str(), log_acks.c_str());
  return true;
}

// One field of an AVR profile — how to query it, which response line to match,
// and what value we want set.
//   query  : full query command including trailing '\r' (e.g. "VSMONI ?\r").
//            Query syntax is inconsistent per-command on Denon X2800H:
//              PW?, MU?, MS?, SI?, PSFRONT?     — NO space before '?'
//              VSMONI ?                          — SPACE required before '?'
//            When adding new fields, confirm empirically via a Telnet probe.
//   prefix : the leading token of the answer line to match against, so we
//            can pick the actual response out of a multi-line reply that
//            may also contain side-effect notifications (e.g. querying
//            PSFRONT returns "SSFRSDST SPA\rPSFRONT SPA\r" — we want the
//            PSFRONT line).
//   target : the set-command payload we want the field to hold (no trailing
//            '\r' — the applier adds it). Also used verbatim to check
//            equality against the current response line.
struct AvrField {
  const char *query;
  const char *prefix;
  const char *target;
};

// Read-modify-write applier: pipeline all queries, read all responses in one
// window, diff each field against target, then pipeline write-commands only
// for fields that actually drifted. Silent when everything matches — no
// dropouts from redundant writes.
//
// Total wall time ≈ (count-1)*inter_cmd_ms + read_ms + (writes-1)*inter_cmd_ms.
// For count=4, inter_cmd=50, read=500: ~700-900 ms cold, ~700 ms if nothing
// drifted. This is close to the old blind-write cost, without the churn.
//
// If a query returns no matching prefix line (AVR was slow or gave junk),
// falls through to writing the target — same at-least-once semantics as the
// original blind-write function. Logged as "was:?" so it's easy to spot.
//
// Returns the number of writes actually issued, or -1 on TCP send failure.
inline int tcp_avr_apply_profile(const char *tag, int sock,
                                  const AvrField *fields, int count,
                                  int inter_cmd_ms = 50,
                                  int read_ms = 500) {
  // Phase 1: pipeline all queries.
  for (int i = 0; i < count; i++) {
    if (!tcp_send(sock, fields[i].query, std::strlen(fields[i].query))) {
      ESP_LOGW(tag, "query send fail: %s", fields[i].prefix);
      return -1;
    }
    if (i + 1 < count && inter_cmd_ms > 0) ::usleep(inter_cmd_ms * 1000);
  }

  // Phase 2: read all responses into one buffer.
  std::string buf;
  tcp_read_all(sock, buf, read_ms);

  // Phase 3: diff. Collect targets that need writing.
  // 8-slot fixed cap — no AVR profile in this project uses more than 4.
  const char *to_write[8];
  int to_write_n = 0;
  std::string skip_list, write_list;
  for (int i = 0; i < count && i < 8; i++) {
    const AvrField &f = fields[i];
    std::string cur;
    size_t start = 0, plen = std::strlen(f.prefix);
    while (start < buf.size()) {
      size_t end = buf.find('\r', start);
      if (end == std::string::npos) end = buf.size();
      if (end - start >= plen &&
          buf.compare(start, plen, f.prefix, plen) == 0) {
        cur = buf.substr(start, end - start);
        break;
      }
      start = end + 1;
    }
    if (cur == f.target) {
      if (!skip_list.empty()) skip_list += ",";
      skip_list += f.target;
    } else {
      if (!write_list.empty()) write_list += ",";
      write_list += std::string(f.target) + "(was:" +
                    (cur.empty() ? "?" : cur) + ")";
      to_write[to_write_n++] = f.target;
    }
  }

  // Phase 4: pipeline the writes.
  for (int i = 0; i < to_write_n; i++) {
    std::string cmd = std::string(to_write[i]) + "\r";
    if (!tcp_send(sock, cmd.c_str(), cmd.size())) {
      ESP_LOGW(tag, "write send fail: %s", to_write[i]);
      return -1;
    }
    if (i + 1 < to_write_n && inter_cmd_ms > 0) ::usleep(inter_cmd_ms * 1000);
  }

  ESP_LOGI(tag, "wrote=%d skipped=[%s] changes=[%s]",
           to_write_n,
           skip_list.empty()  ? "-" : skip_list.c_str(),
           write_list.empty() ? "-" : write_list.c_str());
  return to_write_n;
}
