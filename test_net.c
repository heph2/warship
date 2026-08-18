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
// A readable descriptor does not imply a datagram: juice also posts state
// changes on the same pipe. Keep polling until the deadline, exactly as the
// real loop in main.c does.
static int recv_wait(Net *n, char *buf, int cap, int timeout_ms) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000;
    int left = timeout_ms - (int)elapsed;
    if (left <= 0)
      return 0;

    struct pollfd p[NET_MAX_POLLFDS];
    int nfds = net_pollfds(n, p, NET_MAX_POLLFDS);
    if (poll(p, (nfds_t)nfds, left) < 1)
      return 0;

    int len = net_recv(n, buf, cap);
    if (len > 0)
      return len;
  }
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
  // "relay": skip ICE entirely. A short timeout is not enough -- loopback ICE
  // completes in roughly a millisecond, so the race is not reliably lost.
  int force_relay = argc > 2 && !strcmp(argv[2], "relay");
  const char *want_route = force_relay ? "signal-relay" : "host";
  int code_pipe[2];
  assert(pipe(code_pipe) == 0);

  pid_t pid = fork();
  assert(pid >= 0);

  NetConfig cfg = {.signal_host = "127.0.0.1",
                   .signal_port = port,
                   .timeout_ms = 20000,
                   .ice_timeout_ms = force_relay ? -1 : 20000};
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
    // Checked after the exchange, not before: which side notices first is a
    // race, but once traffic has crossed both must agree on the path.
    if (strcmp(net_route(n), want_route)) {
      fprintf(stderr, "guest: route %s, wanted %s\n", net_route(n), want_route);
      _exit(1);
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
  assert(!strcmp(net_route(n), want_route));
  assert(net_alive(n));
  net_close(n);

  int status = 0;
  waitpid(pid, &status, 0);
  if (!got || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "handshake failed (got=%d child=%d)\n", got, status);
    return 1;
  }
  printf("net ok via %s (room %s)\n", want_route, room);
  return 0;
}
