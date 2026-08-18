#define _POSIX_C_SOURCE 200809L

#include "net.h"
#include "signaling.h"

#include <juice/juice.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Tags on the socketpair. The juice thread writes them, the main loop reads.
#define TAG_DATA 'D'  // a game datagram arrived
#define TAG_CAND 'C'  // we gathered a local candidate, forward it to the peer
#define TAG_DONE 'G'  // local gathering finished
#define TAG_STATE 'S' // ICE state changed

#define MAX_PENDING 4 // early datagrams held during the handshake
#define PROTO_LINE_CAP 512 // one relayed message; proto.c caps its lines at 256

// How data actually reaches the peer.
typedef enum {
  MODE_ICE,  // direct, or via TURN if one was configured
  MODE_RELAY // through the rendezvous server we already have a socket to
} Mode;

struct Net {
  juice_agent_t *agent;
  Mode mode;
  int pipe[2]; // [0] read by main loop, [1] written by the juice thread
  atomic_int state;
  Signal sig;

  // Datagrams that arrived before net_open() returned. Bounded on purpose:
  // dropping past the cap is safe because proto.c retransmits.
  char pend[MAX_PENDING][NET_MAX_DATAGRAM];
  int pendlen[MAX_PENDING];
  int npend;
};

static char last_err[256] = "";

const char *net_error(void) { return last_err; }

static void set_err(const char *s) {
  snprintf(last_err, sizeof last_err, "%.*s", (int)sizeof last_err - 1, s);
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ------------------------------------------------- juice thread callbacks ---

static void push(Net *n, char tag, const char *data, int len) {
  char buf[1 + NET_MAX_DATAGRAM];
  if (len > NET_MAX_DATAGRAM)
    return; // oversized: proto.c would reject it anyway
  buf[0] = tag;
  if (len)
    memcpy(buf + 1, data, (size_t)len);
  ssize_t w = write(n->pipe[1], buf, (size_t)(1 + len));
  (void)w; // a full pipe means we are wedged; retransmission covers the gap
}

static void on_state(juice_agent_t *a, juice_state_t st, void *user) {
  (void)a;
  Net *n = user;
  atomic_store(&n->state, (int)st);
  push(n, TAG_STATE, NULL, 0);
}

static void on_candidate(juice_agent_t *a, const char *sdp, void *user) {
  (void)a;
  push((Net *)user, TAG_CAND, sdp, (int)strlen(sdp));
}

static void on_gathering_done(juice_agent_t *a, void *user) {
  (void)a;
  push((Net *)user, TAG_DONE, NULL, 0);
}

static void on_recv(juice_agent_t *a, const char *data, size_t size, void *user) {
  (void)a;
  push((Net *)user, TAG_DATA, data, (int)size);
}

// ------------------------------------------------------------- SDP on one line ---

// A local description is multi-line; the signaling protocol is line-based.
// Swap newlines for '|' rather than inventing a multi-line framing.
static int sdp_pack(const char *sdp, char *out, int cap) {
  int j = 0;
  for (int i = 0; sdp[i]; i++) {
    if (sdp[i] == '\r')
      continue;
    if (j >= cap - 1)
      return 0;
    out[j++] = (sdp[i] == '\n') ? '|' : sdp[i];
  }
  while (j > 0 && out[j - 1] == '|')
    j--; // trailing newline would become a blank SDP line
  out[j] = '\0';
  return j > 0;
}

static int sdp_unpack(const char *packed, char *out, int cap) {
  int j = 0;
  for (int i = 0; packed[i]; i++) {
    if (j >= cap - 2)
      return 0;
    out[j++] = (packed[i] == '|') ? '\n' : packed[i];
  }
  out[j++] = '\n';
  out[j] = '\0';
  return 1;
}

// ------------------------------------------------------------------- open ---

int net_pollfds(const Net *n, struct pollfd *out, int cap) {
  int i = 0;
  if (i < cap && n->pipe[0] >= 0)
    out[i++] = (struct pollfd){.fd = n->pipe[0], .events = POLLIN};
  if (i < cap && signal_fd(&n->sig) >= 0)
    out[i++] = (struct pollfd){.fd = signal_fd(&n->sig), .events = POLLIN};
  return i;
}

// Give up on a direct path. Not an error: the game carries on over the relay.
//
// Announce it with an empty RELAY line rather than waiting for the first game
// message. Otherwise a peer whose own ICE succeeded keeps sending into a
// session we stopped listening to, and only discovers the truth when its
// retransmits run out.
static void go_relay(Net *n) {
  if (n->mode == MODE_RELAY)
    return;
  n->mode = MODE_RELAY;
  signal_send(&n->sig, "RELAY", ""); // payload-free: a mode switch, not data
}

int net_alive(const Net *n) {
  if (n->mode == MODE_RELAY)
    return signal_fd(&n->sig) >= 0;
  int st = atomic_load((atomic_int *)&n->state);
  return st != JUICE_STATE_FAILED && st != JUICE_STATE_DISCONNECTED;
}

const char *net_route(const Net *n) {
  static char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  static char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  if (n->mode == MODE_RELAY)
    return "signal-relay";
  if (juice_get_selected_candidates(n->agent, local, sizeof local, remote,
                                    sizeof remote) != 0)
    return "unknown";
  if (strstr(local, "typ relay") || strstr(remote, "typ relay"))
    return "relay";
  if (strstr(local, "typ srflx") || strstr(remote, "typ srflx"))
    return "srflx";
  if (strstr(local, "typ host") && strstr(remote, "typ host"))
    return "host";
  return "unknown";
}

static void stash(Net *n, const char *buf, int len) {
  if (n->npend >= MAX_PENDING)
    return;
  memcpy(n->pend[n->npend], buf, (size_t)len);
  n->pendlen[n->npend] = len;
  n->npend++;
}

// Handle one line from the signaling server. 0 means give up. *got_desc is set
// when the peer's description was applied.
static int on_signal_line(Net *n, const char *line, int *got_desc) {
  char sdp[JUICE_MAX_SDP_STRING_LEN];

  // A RELAY line proves the peer gave up on a direct path. Follow it, or we
  // would keep shouting into an ICE session it has stopped listening to.
  if (!strncmp(line, "RELAY ", 6)) {
    go_relay(n);
    return 1;
  }

  if (!strncmp(line, "DESC ", 5)) {
    if (!sdp_unpack(line + 5, sdp, sizeof sdp)) {
      set_err("peer description too long");
      return 0;
    }
    if (juice_set_remote_description(n->agent, sdp) != 0) {
      set_err("peer sent an unusable ICE description");
      return 0;
    }
    *got_desc = 1;
    return 1;
  }
  if (!strncmp(line, "CAND ", 5)) {
    juice_add_remote_candidate(n->agent, line + 5); // a bad one is just ignored
    return 1;
  }
  if (!strcmp(line, "DONE")) {
    juice_set_remote_gathering_done(n->agent);
    return 1;
  }
  if (!strcmp(line, "PEERGONE")) {
    set_err("peer disconnected during setup");
    return 0;
  }
  if (!strncmp(line, "ERR", 3)) {
    set_err(line);
    return 0;
  }
  return 1; // unknown but harmless
}

// Handle one record off the socketpair. 0 means give up.
static int on_pipe_record(Net *n, const char *rec, int len, int connecting) {
  char tag = rec[0];
  const char *body = rec + 1;
  int blen = len - 1;

  switch (tag) {
  case TAG_CAND: {
    char cand[JUICE_MAX_CANDIDATE_SDP_STRING_LEN + 1];
    if (blen >= (int)sizeof cand)
      return 1;
    memcpy(cand, body, (size_t)blen);
    cand[blen] = '\0';
    signal_send(&n->sig, "CAND", cand);
    return 1;
  }
  case TAG_DONE:
    signal_send(&n->sig, "DONE", NULL);
    return 1;
  case TAG_DATA:
    if (connecting)
      stash(n, body, blen); // peer got there first; do not lose the message
    return 1;
  case TAG_STATE: {
    int st = atomic_load(&n->state);
    if (st == JUICE_STATE_FAILED)
      go_relay(n); // no direct path exists; the relay still does
    return 1;
  }
  default:
    return 1;
  }
}

// Publish our ICE description and start gathering candidates.
static int offer(Net *n) {
  char local[JUICE_MAX_SDP_STRING_LEN];
  char packed[JUICE_MAX_SDP_STRING_LEN];
  if (juice_get_local_description(n->agent, local, sizeof local) != 0 ||
      !sdp_pack(local, packed, sizeof packed)) {
    set_err("could not build a local ICE description");
    return 0;
  }
  if (!signal_send(&n->sig, "DESC", packed)) {
    set_err(signal_error());
    return 0;
  }
  juice_gather_candidates(n->agent);
  return 1;
}

int net_open(Net **out, const NetConfig *cfg, char *room_out, int room_cap,
             int *is_host) {
  Net *n = calloc(1, sizeof *n);
  if (!n) {
    set_err("out of memory");
    return 0;
  }
  n->pipe[0] = n->pipe[1] = -1;
  atomic_init(&n->state, JUICE_STATE_DISCONNECTED);

  int timeout = cfg->timeout_ms > 0 ? cfg->timeout_ms : 30000;

  if (!signal_connect(&n->sig, cfg->signal_host, cfg->signal_port)) {
    set_err(signal_error());
    goto fail;
  }

  if (cfg->room) {
    if (!signal_join_room(&n->sig, cfg->room, timeout)) {
      set_err(signal_error());
      goto fail;
    }
    snprintf(room_out, (size_t)room_cap, "%s", cfg->room);
  } else {
    if (!signal_new_room(&n->sig, room_out, room_cap, timeout)) {
      set_err(signal_error());
      goto fail;
    }
  }

  if (cfg->on_room)
    cfg->on_room(room_out, cfg->on_room_ctx);

  // The one place the program is idle on purpose: nothing can happen until a
  // human somewhere else types the code.
  if (!signal_wait_peer(&n->sig, is_host, timeout)) {
    set_err(signal_error());
    goto fail;
  }

  // SOCK_DGRAM, not SOCK_STREAM: record boundaries must survive, otherwise two
  // datagrams written by the juice thread would arrive glued together.
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, n->pipe) != 0) {
    set_err(strerror(errno));
    goto fail;
  }
  // Non-blocking read end: net_recv() drains records in a loop and must be able
  // to discover "nothing left" without stalling the game.
  fcntl(n->pipe[0], F_SETFL, O_NONBLOCK);

  juice_turn_server_t turn = {0};
  juice_config_t jc = {
      .concurrency_mode = JUICE_CONCURRENCY_MODE_POLL,
      .stun_server_host = cfg->stun_host,
      .stun_server_port = (unsigned short)cfg->stun_port,
      .cb_state_changed = on_state,
      .cb_candidate = on_candidate,
      .cb_gathering_done = on_gathering_done,
      .cb_recv = on_recv,
      .user_ptr = n,
  };
  if (cfg->turn_host) {
    turn.host = cfg->turn_host;
    turn.port = (unsigned short)cfg->turn_port;
    turn.username = cfg->turn_user;
    turn.password = cfg->turn_pass;
    jc.turn_servers = &turn;
    jc.turn_servers_count = 1;
  }

  n->agent = juice_create(&jc);
  if (!n->agent) {
    set_err("juice_create failed");
    goto fail;
  }

  if (cfg->ice_timeout_ms < 0)
    go_relay(n); // caller does not want a direct path at all

  // Whoever describes itself first becomes the ICE controlling agent. If both
  // do it, libjuice logs a role conflict and has to resolve it by tie-breaker.
  // The host offers; the guest answers only after applying the offer.
  int desc_sent = 0, remote_desc = 0;
  if (n->mode == MODE_ICE && *is_host && !offer(n))
    goto fail;
  desc_sent = *is_host;

  // The wait for a human to type the code is over; from here we are only
  // waiting on machines, and a much shorter budget applies. Running out is not
  // a failure -- it is the signal to relay instead.
  long long deadline =
      now_ms() + (cfg->ice_timeout_ms > 0 ? cfg->ice_timeout_ms : 20000);
  for (;;) {
    int st = atomic_load(&n->state);
    if (st == JUICE_STATE_COMPLETED || n->mode == MODE_RELAY)
      break;

    if (now_ms() >= deadline) {
      go_relay(n);
      break;
    }
    int wait = (int)(deadline - now_ms());

    struct pollfd fds[2] = {
        {.fd = signal_fd(&n->sig), .events = POLLIN},
        {.fd = n->pipe[0], .events = POLLIN},
    };
    int rc = poll(fds, 2, wait > 250 ? 250 : wait);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      set_err(strerror(errno));
      goto fail;
    }

    if (fds[0].revents & POLLIN) {
      char line[SIGNAL_LINE];
      int lr = signal_line(&n->sig, line, sizeof line, 0);
      if (lr < 0) {
        set_err(signal_error());
        goto fail;
      }
      if (lr == 1 && !on_signal_line(n, line, &remote_desc))
        goto fail;
      if (!desc_sent && remote_desc) {
        if (!offer(n))
          goto fail;
        desc_sent = 1;
      }
    }

    if (fds[1].revents & POLLIN) {
      char rec[1 + NET_MAX_DATAGRAM];
      ssize_t r = read(n->pipe[0], rec, sizeof rec);
      if (r > 0 && !on_pipe_record(n, rec, (int)r, 1))
        goto fail;
    }
  }

  // The signaling socket deliberately stays open for the whole game: it is the
  // relay of last resort, and it costs one idle TCP connection to keep.
  *out = n;
  return 1;

fail:
  if (n->agent)
    juice_destroy(n->agent);
  signal_close(&n->sig);
  if (n->pipe[0] >= 0)
    close(n->pipe[0]);
  if (n->pipe[1] >= 0)
    close(n->pipe[1]);
  free(n);
  return 0;
}

// ------------------------------------------------------------------- data ---

// Relayed payloads travel as one signaling line, so the trailing newline that
// proto.c puts on every message has to come off and go back on again.
static int relay_send(Net *n, const char *buf, int len) {
  char payload[PROTO_LINE_CAP];
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    len--;
  if (len <= 0 || len >= (int)sizeof payload)
    return 0;
  memcpy(payload, buf, (size_t)len);
  payload[len] = '\0';
  return signal_send(&n->sig, "RELAY", payload);
}

int net_send(Net *n, const char *buf, int len) {
  if (n->mode == MODE_ICE) {
    int st = atomic_load(&n->state);
    if (st == JUICE_STATE_FAILED || st == JUICE_STATE_DISCONNECTED)
      go_relay(n);
  }
  if (n->mode == MODE_RELAY)
    return relay_send(n, buf, len);
  return juice_send(n->agent, buf, (size_t)len) == 0;
}

int net_recv(Net *n, char *buf, int cap) {
  if (n->npend > 0) { // handshake leftovers come out first, in order
    int len = n->pendlen[0];
    if (len > cap)
      len = cap;
    memcpy(buf, n->pend[0], (size_t)len);
    memmove(n->pend[0], n->pend[1], sizeof n->pend[0] * (size_t)(--n->npend));
    memmove(n->pendlen, n->pendlen + 1, sizeof(int) * (size_t)n->npend);
    return len;
  }

  for (;;) {
    char rec[1 + NET_MAX_DATAGRAM];
    ssize_t r = read(n->pipe[0], rec, sizeof rec);
    if (r <= 0)
      break; // the ICE side is drained
    if (rec[0] == TAG_DATA) {
      int len = (int)r - 1;
      if (len > cap)
        len = cap;
      memcpy(buf, rec + 1, (size_t)len);
      return len;
    }
    on_pipe_record(n, rec, (int)r, 0); // state changes and late candidates
  }

  // Then the relay. Reading this even while ICE looks healthy is what lets a
  // peer that has already given up drag us over to the relay with it.
  for (;;) {
    char line[SIGNAL_LINE];
    int lr = signal_line(&n->sig, line, sizeof line, 0);
    if (lr == 0)
      return 0;
    if (lr < 0) {
      signal_close(&n->sig);
      return 0;
    }
    if (!strncmp(line, "RELAY ", 6)) {
      go_relay(n);
      int len = (int)strlen(line + 6);
      if (len == 0)
        continue; // bare marker: the peer switched, there is no datagram here
      if (len > cap)
        len = cap;
      memcpy(buf, line + 6, (size_t)len);
      return len;
    }
    if (!strcmp(line, "PEERGONE") || !strncmp(line, "ERR", 3)) {
      set_err(line);
      signal_close(&n->sig);
      return 0;
    }
    int ignored = 0;
    on_signal_line(n, line, &ignored); // late trickled candidates
  }
}

void net_close(Net *n) {
  if (!n)
    return;
  if (n->agent)
    juice_destroy(n->agent); // joins the juice thread before the pipe closes
  signal_close(&n->sig);
  if (n->pipe[0] >= 0)
    close(n->pipe[0]);
  if (n->pipe[1] >= 0)
    close(n->pipe[1]);
  free(n);
}
