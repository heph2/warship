// Integration test for signal.c: needs a live rendezvous server, so it is not
// part of `make test`. Run it with `make itest`, which starts one.
#include "signaling.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *port = argc > 1 ? argv[1] : "17778";
  Signal host, guest;
  char code[SIGNAL_CODE_LEN + 1];
  char line[SIGNAL_LINE];
  int is_host;

  assert(signal_connect(&host, "127.0.0.1", port));
  assert(signal_new_room(&host, code, sizeof code, 2000));
  assert(strlen(code) == SIGNAL_CODE_LEN);

  assert(signal_connect(&guest, "127.0.0.1", port));
  assert(signal_join_room(&guest, code, 2000));

  assert(signal_wait_peer(&host, &is_host, 2000) && is_host == 1);
  assert(signal_wait_peer(&guest, &is_host, 2000) && is_host == 0);

  assert(signal_send(&host, "DESC", "a=ice-ufrag:abcd"));
  assert(signal_line(&guest, line, sizeof line, 2000) == 1);
  assert(!strcmp(line, "DESC a=ice-ufrag:abcd"));

  assert(signal_send(&guest, "CAND", "candidate:1 1 UDP 2130706431 192.168.1.9 4444 typ host"));
  assert(signal_line(&host, line, sizeof line, 2000) == 1);
  assert(!strncmp(line, "CAND candidate:1", 16));

  // No traffic pending: must time out cleanly rather than block or lie.
  assert(signal_line(&host, line, sizeof line, 200) == 0);

  // Bad code is rejected by the server, surfaced through signal_error().
  Signal bad;
  assert(signal_connect(&bad, "127.0.0.1", port));
  assert(signal_join_room(&bad, "zzzzzz", 2000));
  assert(signal_wait_peer(&bad, &is_host, 2000) == 0);
  assert(strstr(signal_error(), "nosuchroom"));

  // Peer hangs up -> the other side learns, does not hang.
  signal_close(&guest);
  assert(signal_line(&host, line, sizeof line, 2000) == 1);
  assert(!strcmp(line, "PEERGONE"));

  signal_close(&host);
  printf("signal client ok (room %s)\n", code);
  return 0;
}
