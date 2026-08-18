// Integration test for the WebSocket signaling client and server. Needs a live
// signal-server, so it runs under `make itest`, which starts one.
//
// One client per process, like the real game: libwebsockets drives its own
// event loop per context, and interleaving several contexts in one thread is a
// shape the product never has.
#define _POSIX_C_SOURCE 200809L

#include "signaling.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char url[128];

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Retry briefly: the harness backgrounds the server and starts us immediately,
// so the very first connection can arrive before it is listening.
static Signal *open_client(void) {
  for (int attempt = 0; attempt < 10; attempt++) {
    Signal *s = NULL;
    if (signal_connect(&s, url, 2000))
      return s;
    struct timespec ts = {.tv_nsec = 200000000};
    nanosleep(&ts, NULL);
  }
  fprintf(stderr, "connect failed: %s\n", signal_error());
  return NULL;
}

// Pump this client until a message arrives, or the clock runs out.
static int expect(Signal *s, char *out, int cap, int timeout_ms) {
  long long deadline = now_ms() + timeout_ms;
  do {
    if (signal_next(s, out, cap))
      return 1;
    signal_service(s, 20);
  } while (now_ms() < deadline);
  return signal_next(s, out, cap);
}

static int expect_exact(Signal *s, const char *want, int timeout_ms) {
  char msg[SIGNAL_MSG_MAX + 1];
  if (!expect(s, msg, sizeof msg, timeout_ms)) {
    fprintf(stderr, "timed out waiting for '%s' (alive=%d, %s)\n", want,
            signal_alive(s), signal_error());
    return 0;
  }
  if (strcmp(msg, want)) {
    fprintf(stderr, "got '%s', wanted '%s'\n", msg, want);
    return 0;
  }
  return 1;
}

// Everything a client can get wrong. Each case gets its own connection, so
// there is only ever one websocket context alive at a time.
static int test_rejections(void) {
  struct {
    const char *send;
    const char *want;
  } bad[] = {
      {"JOIN zz", "ERROR malformed room code"},
      {"JOIN qqqqqq", "ERROR no such room"},
      {"HACK me", "ERROR unknown message"},
      {"SIGNAL \x1b[2J", "ERROR payload rejected"},
      {"SIGNAL orphan", "ERROR no peer in room"},
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    Signal *d = open_client();
    if (!d)
      return 0;
    if (!signal_send(d, bad[i].send) || !expect_exact(d, bad[i].want, 5000)) {
      fprintf(stderr, "case '%s' failed\n", bad[i].send);
      signal_close(d);
      return 0;
    }
    signal_close(d);
  }
  return 1;
}

static int guest_side(const char *code) {
  Signal *g = open_client();
  if (!g)
    return 1;

  int is_host = 1;
  if (!signal_join_room(g, code) || !signal_wait_peer(g, &is_host, 10000) ||
      is_host != 0) {
    fprintf(stderr, "guest: pairing failed: %s\n", signal_error());
    return 1;
  }

  // A multi-line SDP survives intact: websocket frames are length-delimited,
  // so nothing has to be escaped on the way through.
  const char *sdp = "SIGNAL Da=ice-ufrag:abcd\na=ice-pwd:secret\n";
  if (!expect_exact(g, sdp, 10000))
    return 1;
  if (!signal_send(g, "SIGNAL Ccandidate:1 1 UDP 100 10.0.0.1 5000 typ host"))
    return 1;

  // Hold the room open long enough for the host to see the candidate, then
  // vanish, which is what the host's PEERGONE check needs.
  for (int i = 0; i < 40; i++)
    signal_service(g, 20);
  signal_close(g);
  return 0;
}

int main(int argc, char **argv) {
  const char *port = argc > 1 ? argv[1] : "17778";
  snprintf(url, sizeof url, "ws://127.0.0.1:%s/", port);

  char msg[SIGNAL_MSG_MAX + 1];
  char code[SIGNAL_CODE_LEN + 1];
  int is_host = 0;

  assert(test_rejections());

  Signal *host = open_client();
  assert(host);
  assert(signal_create_room(host, code, sizeof code, 5000));
  assert(strlen(code) == SIGNAL_CODE_LEN);

  int codepipe[2];
  assert(pipe(codepipe) == 0);
  pid_t pid = fork();
  assert(pid >= 0);

  if (pid == 0) {
    signal_close(host); // the child gets its own connection
    close(codepipe[1]);
    char got[SIGNAL_CODE_LEN + 1] = {0};
    ssize_t r = read(codepipe[0], got, SIGNAL_CODE_LEN);
    close(codepipe[0]);
    _exit(r == SIGNAL_CODE_LEN ? guest_side(got) : 1);
  }

  close(codepipe[0]);
  ssize_t w = write(codepipe[1], code, SIGNAL_CODE_LEN);
  (void)w;
  close(codepipe[1]);

  assert(signal_wait_peer(host, &is_host, 10000) && is_host == 1);
  assert(signal_send(host, "SIGNAL Da=ice-ufrag:abcd\na=ice-pwd:secret\n"));
  assert(expect(host, msg, sizeof msg, 10000));
  assert(!strncmp(msg, "SIGNAL Ccandidate:1", 19));

  // The guest leaving must be reported, not silently endured.
  assert(expect_exact(host, "PEERGONE", 10000));
  signal_close(host);

  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // The room went with it: the code cannot be reused.
  Signal *late = open_client();
  assert(late);
  assert(signal_join_room(late, code));
  assert(expect_exact(late, "ERROR no such room", 5000));
  signal_close(late);

  printf("signaling ok (room %s)\n", code);
  return 0;
}
