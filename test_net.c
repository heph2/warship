#define _POSIX_C_SOURCE 200809L

// Integration test for net.c: forks a host and a guest, connects them through a
// local rendezvous server over loopback ICE, and moves real datagrams. Needs a
// running server, so it lives behind `make itest`, not `make test`.
#include "net.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Wait for one datagram, or give up. Loss is possible in principle even on
// loopback, so the sender repeats and this just needs the first arrival.
static int recv_wait(Net *n, char *buf, int cap, int timeout_ms) {
  struct pollfd p = {.fd = net_fd(n), .events = POLLIN};
  if (poll(&p, 1, timeout_ms) != 1)
    return 0;
  return net_recv(n, buf, cap);
}

static int code_out_fd = -1;

static void publish_code(const char *code, void *ctx) {
  (void)ctx;
  ssize_t w = write(code_out_fd, code, strlen(code));
  (void)w;
  close(code_out_fd);
}

int main(int argc, char **argv) {
  const char *port = argc > 1 ? argv[1] : "17779";
  int code_pipe[2];
  assert(pipe(code_pipe) == 0);

  pid_t pid = fork();
  assert(pid >= 0);

  NetConfig cfg = {.signal_host = "127.0.0.1",
                   .signal_port = port,
                   .timeout_ms = 20000};
  char room[64] = "";
  int is_host = -1;
  Net *n = NULL;
  char buf[NET_MAX_DATAGRAM];

  if (pid == 0) { // guest: learn the code from the parent, then join
    close(code_pipe[1]);
    char code[16] = {0};
    ssize_t r = read(code_pipe[0], code, sizeof code - 1);
    assert(r > 0);
    close(code_pipe[0]);

    cfg.room = code;
    if (!net_open(&n, &cfg, room, sizeof room, &is_host)) {
      fprintf(stderr, "guest: %s\n", net_error());
      _exit(1);
    }
    assert(is_host == 0);

    int len = recv_wait(n, buf, sizeof buf, 5000);
    if (len <= 0 || strncmp(buf, "0 HELLO 1 host", (size_t)len)) {
      fprintf(stderr, "guest: bad first datagram (%d bytes)\n", len);
      _exit(1);
    }
    for (int i = 0; i < 5; i++) { // unreliable: repeat, the peer dedupes
      net_send(n, "0 HELLO 1 guest", 15);
      struct timespec ts = {.tv_nsec = 50000000};
      nanosleep(&ts, NULL);
    }
    net_close(n);
    _exit(0);
  }

  close(code_pipe[0]); // host: make a room, hand the code over, connect
  cfg.room = NULL;
  code_out_fd = code_pipe[1];
  cfg.on_room = publish_code;
  if (!net_open(&n, &cfg, room, sizeof room, &is_host)) {
    fprintf(stderr, "host: %s\n", net_error());
    return 1;
  }
  assert(is_host == 1);
  assert(strlen(room) == 6);

  // Deliberately racy on purpose: the guest may still be finishing ICE, so the
  // host repeats until it hears back. This is exactly why proto.c exists.
  int got = 0;
  for (int i = 0; i < 200 && !got; i++) { // 20s, matching the guest's ICE budget
    net_send(n, "0 HELLO 1 host", 14);
    int len = recv_wait(n, buf, sizeof buf, 100);
    if (len > 0 && !strncmp(buf, "0 HELLO 1 guest", (size_t)len))
      got = 1;
  }

  printf("route: %s\n", net_route(n));
  assert(net_alive(n));
  net_close(n);

  int status = 0;
  waitpid(pid, &status, 0);
  if (!got || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "handshake failed (got=%d child=%d)\n", got, status);
    return 1;
  }
  printf("net ok (room %s)\n", room);
  return 0;
}
