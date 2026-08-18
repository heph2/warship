#define _POSIX_C_SOURCE 200809L

#include "signaling.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char last_err[128] = "";

const char *signal_error(void) { return last_err; }

static void set_err(const char *s) {
  // Explicit precision: s is never a format string, and truncating a long
  // server message is fine -- silently, not with a warning.
  snprintf(last_err, sizeof last_err, "%.*s", (int)sizeof last_err - 1, s);
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int signal_fd(const Signal *s) { return s->fd; }

void signal_close(Signal *s) {
  if (s->fd >= 0)
    close(s->fd);
  s->fd = -1;
  s->inlen = 0;
}

int signal_connect(Signal *s, const char *host, const char *port) {
  s->fd = -1;
  s->inlen = 0;

  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
  struct addrinfo *res;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) {
    set_err(gai_strerror(rc));
    return 0;
  }

  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      s->fd = fd;
      break;
    }
    close(fd);
  }
  freeaddrinfo(res);

  if (s->fd < 0) {
    set_err(strerror(errno));
    return 0;
  }
  return 1;
}

int signal_send(Signal *s, const char *verb, const char *payload) {
  if (s->fd < 0)
    return 0;

  char line[SIGNAL_LINE + 2];
  int n = payload ? snprintf(line, sizeof line, "%s %s\n", verb, payload)
                  : snprintf(line, sizeof line, "%s\n", verb);
  if (n <= 0 || n >= (int)sizeof line) {
    set_err("line too long");
    return 0; // refuse to emit a truncated line rather than confuse the server
  }

  int off = 0;
  while (off < n) {
    // MSG_NOSIGNAL, not write(): once this socket also carries relayed game
    // traffic, a server or peer that vanishes would otherwise kill us with
    // SIGPIPE mid-game. Targeted, so we do not touch the process-wide
    // disposition on the caller's behalf.
    ssize_t w = send(s->fd, line + off, (size_t)(n - off), MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      set_err(strerror(errno));
      return 0;
    }
    off += (int)w;
  }
  return 1;
}

// One line out of the buffer, if a complete one is there.
static int take_line(Signal *s, char *out, int cap) {
  char *nl = memchr(s->in, '\n', (size_t)s->inlen);
  if (!nl)
    return 0;

  int len = (int)(nl - s->in);
  if (len > 0 && s->in[len - 1] == '\r')
    len--;
  if (len >= cap)
    len = cap - 1;
  memcpy(out, s->in, (size_t)len);
  out[len] = '\0';

  int consumed = (int)(nl - s->in) + 1;
  memmove(s->in, s->in + consumed, (size_t)(s->inlen - consumed));
  s->inlen -= consumed;
  return 1;
}

int signal_line(Signal *s, char *out, int cap, int timeout_ms) {
  if (s->fd < 0)
    return -1;
  if (take_line(s, out, cap))
    return 1;

  long long deadline = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;
  for (;;) {
    int wait = -1;
    if (deadline >= 0) {
      wait = (int)(deadline - now_ms());
      if (wait < 0)
        return 0;
    }

    struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
    int rc = poll(&pfd, 1, wait);
    if (rc == 0)
      return 0;
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      set_err(strerror(errno));
      return -1;
    }

    if (s->inlen >= SIGNAL_LINE) {
      set_err("server line too long");
      return -1; // never grow the buffer on someone else's say-so
    }
    ssize_t n = read(s->fd, s->in + s->inlen, (size_t)(SIGNAL_LINE - s->inlen));
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      set_err("signaling connection closed");
      return -1;
    }
    s->inlen += (int)n;

    if (take_line(s, out, cap))
      return 1;
  }
}

// An ERR line is the server telling us why it is about to hang up.
static int is_err(const char *line) {
  if (strncmp(line, "ERR", 3) == 0) {
    set_err(line);
    return 1;
  }
  return 0;
}

int signal_new_room(Signal *s, char *code, int cap, int timeout_ms) {
  if (cap <= SIGNAL_CODE_LEN)
    return 0;
  if (!signal_send(s, "NEW", NULL))
    return 0;

  char line[SIGNAL_LINE];
  if (signal_line(s, line, sizeof line, timeout_ms) != 1)
    return 0;
  if (is_err(line))
    return 0;
  if (strncmp(line, "ROOM ", 5) != 0 ||
      (int)strlen(line + 5) != SIGNAL_CODE_LEN) {
    set_err("bad ROOM reply");
    return 0;
  }
  memcpy(code, line + 5, SIGNAL_CODE_LEN + 1);

  if (signal_line(s, line, sizeof line, timeout_ms) != 1 ||
      strcmp(line, "WAITING") != 0) {
    set_err("bad WAITING reply");
    return 0;
  }
  return 1;
}

int signal_join_room(Signal *s, const char *code, int timeout_ms) {
  (void)timeout_ms;
  if ((int)strlen(code) != SIGNAL_CODE_LEN) {
    set_err("room codes are 6 characters");
    return 0;
  }
  return signal_send(s, "JOIN", code);
}

int signal_wait_peer(Signal *s, int *is_host, int timeout_ms) {
  char line[SIGNAL_LINE];
  int rc = signal_line(s, line, sizeof line, timeout_ms);
  if (rc == 0)
    set_err("nobody joined");
  if (rc != 1)
    return 0;
  if (is_err(line))
    return 0;

  if (!strcmp(line, "PEER host")) {
    *is_host = 1;
    return 1;
  }
  if (!strcmp(line, "PEER guest")) {
    *is_host = 0;
    return 1;
  }
  set_err("expected PEER");
  return 0;
}
