#define _POSIX_C_SOURCE 200809L

// Integration test for net.c: forks a host and a guest, connects them through a
// local rendezvous server over loopback ICE, and moves real datagrams. Needs a
// running server, so it lives behind `make itest`, not `make test`.
#include "net.h"
#include "signaling.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Wait for one datagram, or give up. Loss is possible in principle even on
// loopback, so the sender repeats and this just needs the first arrival.
static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// A readable descriptor does not imply a datagram: juice also posts state
// changes on the same pipe. Keep polling until the deadline, exactly as the
// real loop in main.c does.
static int recv_wait(Net *n, char *buf, int cap, int timeout_ms) {
  long long deadline = now_ms() + timeout_ms;
  for (;;) {
    int left = (int)(deadline - now_ms());
    if (left <= 0)
      return 0;

    struct pollfd p = {.fd = net_fd(n), .events = POLLIN};
    if (poll(&p, 1, left) < 1)
      return 0;

    int len = net_recv(n, buf, cap);
    if (len > 0)
      return len;
  }
}

// Pure string handling: no router, no server, no sockets.
static int unit_tests(void) {
  char ip[64], out[512];
  int port;

  assert(net_parse_host_candidate(
      "a=candidate:1 1 UDP 2130706431 192.168.1.9 51234 typ host", ip,
      sizeof ip, &port));
  assert(!strcmp(ip, "192.168.1.9") && port == 51234);

  // No "a=" prefix is equally valid.
  assert(net_parse_host_candidate(
      "candidate:2 1 UDP 2130706431 10.0.0.4 6000 typ host", ip, sizeof ip,
      &port));
  assert(!strcmp(ip, "10.0.0.4") && port == 6000);

  // Nothing here is worth asking a router about.
  assert(!net_parse_host_candidate(
      "a=candidate:1 1 UDP 100 203.0.113.7 9 typ srflx", ip, sizeof ip, &port));
  assert(!net_parse_host_candidate(
      "a=candidate:1 1 UDP 100 fe80::1 9 typ host", ip, sizeof ip, &port));
  assert(!net_parse_host_candidate(
      "a=candidate:1 1 UDP 100 127.0.0.1 9 typ host", ip, sizeof ip, &port));
  assert(!net_parse_host_candidate("garbage", ip, sizeof ip, &port));

  assert(net_mapped_candidate(
      "a=candidate:1 1 UDP 2130706431 192.168.1.9 51234 typ host", "203.0.113.7",
      40000, out, sizeof out));
  assert(!strcmp(out, "a=candidate:map1 1 UDP 1694498815 203.0.113.7 40000 "
                      "typ srflx raddr 192.168.1.9 rport 51234"));

  // The a= prefix is carried through, or not, exactly as it arrived.
  assert(net_mapped_candidate(
      "candidate:7 2 UDP 2130706431 10.0.0.4 6000 typ host", "198.51.100.2",
      1234, out, sizeof out));
  assert(!strncmp(out, "candidate:map7 2 UDP", 20));
  assert(strstr(out, "rport 6000"));

  assert(!net_mapped_candidate("nonsense", "1.2.3.4", 1, out, sizeof out));
  assert(!net_mapped_candidate(
      "a=candidate:1 1 UDP 2130706431 192.168.1.9 51234 typ host", "1.2.3.4", 1,
      out, 20)); // refuses to emit a truncated candidate

  printf("net unit tests ok\n");
  return 0;
}

static char signal_url[128];
static int code_out_fd = -1;

static void publish_code(const char *code, void *ctx) {
  (void)ctx;
  ssize_t w = write(code_out_fd, code, strlen(code));
  (void)w;
  close(code_out_fd);
}

// Retry briefly: under the load of a full itest run the first connection can
// arrive before the freshly started server is listening. A child that gives up
// here closes its socket, and the peer misreads that as the player leaving.
static Signal *open_signal(void) {
  for (int attempt = 0; attempt < 10; attempt++) {
    Signal *s = NULL;
    if (signal_connect(&s, signal_url, 2000))
      return s;
    struct timespec ts = {.tv_nsec = 200000000};
    nanosleep(&ts, NULL);
  }
  fprintf(stderr, "could not reach the signaling service: %s\n", signal_error());
  return NULL;
}

// The spec's hard requirement: no game packet may travel over signaling. The
// strongest local proof is that the websocket is gone once ICE is up -- if the
// room no longer exists, nothing can be forwarded through it.
static int signaling_is_closed(const char *code) {
  Signal *s = open_signal();
  if (!s)
    return 0;
  if (!signal_join_room(s, code)) {
    signal_close(s);
    return 0;
  }

  char msg[SIGNAL_MSG_MAX + 1];
  int ok = 0;
  for (long long deadline = now_ms() + 5000; now_ms() < deadline;) {
    if (signal_next(s, msg, sizeof msg)) {
      ok = !strcmp(msg, "ERROR no such room");
      break;
    }
    signal_service(s, 20);
  }
  signal_close(s);
  return ok;
}

// A peer that pairs but never speaks ICE. The other side must give up with a
// clean error instead of hanging or inventing a fallback.
static int deaf_peer(const char *code) {
  Signal *s = open_signal();
  if (!s)
    return 1;
  int is_host = 1;
  if (!signal_join_room(s, code) || !signal_wait_peer(s, &is_host, 10000)) {
    fprintf(stderr, "deaf peer could not pair: %s\n", signal_error());
    return 1;
  }
  // Stay connected, stay silent, and outlive the other side's ICE budget by a
  // clear margin so the timeout path is what gets exercised.
  long long deadline = now_ms() + 8000;
  while (now_ms() < deadline) {
    if (signal_service(s, 20) < 0) {
      fprintf(stderr, "deaf peer: signaling dropped after %lldms: %s\n",
              8000 - (deadline - now_ms()), signal_error());
      return 1;
    }
  }
  signal_close(s);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "unit"))
    return unit_tests();

  // A bare port means the local harness; a full URL lets the same test run
  // against the real deployment.
  const char *target = argc > 1 ? argv[1] : "17779";
  int expect_failure = argc > 2 && !strcmp(argv[2], "nopath");
  if (!strncmp(target, "ws://", 5) || !strncmp(target, "wss://", 6))
    snprintf(signal_url, sizeof signal_url, "%s", target);
  else
    snprintf(signal_url, sizeof signal_url, "ws://127.0.0.1:%s/", target);

  int code_pipe[2];
  assert(pipe(code_pipe) == 0);

  pid_t pid = fork();
  assert(pid >= 0);

  NetConfig cfg = {.signal_url = signal_url,
                   .timeout_ms = 20000,
                   .ice_timeout_ms = expect_failure ? 3000 : 20000};
  char room[64] = "";
  int is_host = -1;
  Net *n = NULL;
  char buf[NET_MAX_DATAGRAM];

  if (pid == 0) { // guest: learn the code from the parent, then join
    close(code_pipe[1]);
    char code[16] = {0};
    ssize_t r = read(code_pipe[0], code, sizeof code - 1);
    close(code_pipe[0]);
    if (r <= 0)
      _exit(1); // the parent never got a room; it reports the real error

    if (expect_failure)
      _exit(deaf_peer(code));

    cfg.room = code;
    if (!net_open(&n, &cfg, room, sizeof room, &is_host)) {
      fprintf(stderr, "guest: %s\n", net_error());
      _exit(1);
    }
    assert(is_host == 0);

    int len = recv_wait(n, buf, sizeof buf, 10000);
    if (len <= 0 || strncmp(buf, "0 HELLO 1 host", (size_t)len)) {
      fprintf(stderr, "guest: bad first datagram (%d bytes)\n", len);
      _exit(1);
    }
    for (int i = 0; i < 5; i++) { // unreliable: repeat, the peer dedupes
      net_send(n, "0 HELLO 1 guest", 15);
      struct timespec ts = {.tv_nsec = 50000000};
      nanosleep(&ts, NULL);
    }
    const char *groute = net_route(n);
    for (long long deadline = now_ms() + 3000;
         !strcmp(groute, "unknown") && now_ms() < deadline;) {
      struct timespec ts = {.tv_nsec = 50000000};
      nanosleep(&ts, NULL);
      groute = net_route(n);
    }
    if (!strcmp(groute, "unknown")) {
      fprintf(stderr, "guest: no route reported\n");
      _exit(1);
    }
    net_close(n);
    _exit(0);
  }

  close(code_pipe[0]); // host: make a room, hand the code over, connect
  cfg.room = NULL;
  code_out_fd = code_pipe[1];
  cfg.on_room = publish_code;

  if (expect_failure) {
    // No ICE will ever happen, and there is no relay to fall back to.
    if (net_open(&n, &cfg, room, sizeof room, &is_host)) {
      fprintf(stderr, "expected failure, but connected via %s\n", net_route(n));
      return 1;
    }
    // Either clean failure is acceptable. What is being tested is that no
    // path means an error rather than a silent fallback; whether we notice by
    // running out of ICE budget or by the room collapsing first is a race we
    // do not control, and both leave the game refusing to start.
    if (!strstr(net_error(), "timed out") &&
        !strstr(net_error(), "signaling service")) {
      fprintf(stderr, "wrong error: %s\n", net_error());
      return 1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("no-path failure clean: %s\n", net_error());
    return 0;
  }

  if (!net_open(&n, &cfg, room, sizeof room, &is_host)) {
    fprintf(stderr, "host: %s\n", net_error());
    return 1;
  }
  assert(is_host == 1);
  assert(strlen(room) == 6);

  // Deliberately racy: the guest may still be finishing ICE, so the host
  // repeats until it hears back. This is exactly why proto.c exists.
  int got = 0;
  for (int i = 0; i < 200 && !got; i++) {
    net_send(n, "0 HELLO 1 host", 14);
    int len = recv_wait(n, buf, sizeof buf, 100);
    if (len > 0 && !strncmp(buf, "0 HELLO 1 guest", (size_t)len))
      got = 1;
  }

  // The selected pair can take a moment to become readable after COMPLETED,
  // so let the route settle instead of latching whatever the first call says.
  // Sleep between attempts: spinning on net_route() hammers libjuice's lock
  // and starves the very thread that has to publish the selected pair.
  const char *route = net_route(n);
  for (long long deadline = now_ms() + 3000;
       !strcmp(route, "unknown") && now_ms() < deadline;) {
    struct timespec ts = {.tv_nsec = 50000000};
    nanosleep(&ts, NULL);
    route = net_route(n);
  }

  printf("route: %s\n", route);
  assert(strcmp(route, "unknown"));
  assert(net_alive(n));

  int closed = signaling_is_closed(room);
  net_close(n);

  int status = 0;
  waitpid(pid, &status, 0);
  if (!got || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "handshake failed (got=%d child=%d)\n", got, status);
    return 1;
  }
  if (!closed) {
    fprintf(stderr, "signaling room still open after ICE connected\n");
    return 1;
  }
  printf("net ok, signaling closed after connect (room %s)\n", room);
  return 0;
}
