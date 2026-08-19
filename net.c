#define _POSIX_C_SOURCE 200809L

#include "net.h"
#include "signaling.h"

#include <juice/juice.h>
#include <plum/plum.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
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
#define TAG_MAP 'M'   // the router gave us an external ip:port

#define MAX_PENDING 4 // early datagrams held during the handshake

// Payload tags inside a SIGNAL message. The signaling service never looks at
// these -- to it the payload is opaque, which is exactly the point.
#define SIG_DESC 'D'
#define SIG_CAND 'C'
#define SIG_DONE 'G'

struct Net {
  juice_agent_t *agent;

  // Port mapping. libjuice speaks STUN but never asks the router directly, so
  // a NAT that would happily open a port never gets asked. libplum asks, over
  // NAT-PMP, PCP and UPnP-IGD, and the result is advertised to the peer as one
  // more candidate. This is what keeps games off the relay.
  int mapping_id;
  int mapping_asked;
  char host_cand[JUICE_MAX_CANDIDATE_SDP_STRING_LEN + 1]; // template to rewrite
  char host_ip[64];
  int host_port;
  int pipe[2]; // [0] read by main loop, [1] written by the juice thread
  atomic_int state;
  Signal *sig; // closed as soon as ICE connects; never carries game traffic

  // Signaling ending is not a connection failure. Whoever completes ICE first
  // closes its websocket, which collapses the room and hands the other peer a
  // PEERGONE while it is still finishing. All it really means is that no
  // further candidates will arrive.
  int signaling_done;
  char signaling_reason[128];

  // Datagrams that arrived before net_open() returned. Bounded on purpose:
  // dropping past the cap is safe because proto.c retransmits.
  char pend[MAX_PENDING][NET_MAX_DATAGRAM];
  int pendlen[MAX_PENDING];
  int npend;
};

// Defined below, next to offer(): used by the pipe handlers above it.
static int send_signal(Net *n, char tag, const char *body);

static char last_err[256] = "";
static int verbose = 0;

const char *net_error(void) { return last_err; }
void net_set_verbose(int v) { verbose = v; }

static void set_err(const char *s) {
  snprintf(last_err, sizeof last_err, "%.*s", (int)sizeof last_err - 1, s);
}

// Same rule as signaling.c: stderr, never stdout, because ui.c owns the whole
// screen and a stray line there would corrupt the next frame. This is called
// only from the main thread (on_pipe_record, on_signal_msg, net_open), never
// from a libjuice/libplum callback -- those may only write to the pipe.
static void vlog(const char *fmt, ...) {
  if (!verbose)
    return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[net] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
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

// Also a foreign thread. Same rule: write to the pipe, touch nothing else.
static void on_mapping(int id, plum_state_t state, const plum_mapping_t *mapping) {
  (void)id;
  if (state != PLUM_STATE_SUCCESS || !mapping || !mapping->user_ptr)
    return;
  char line[PLUM_MAX_HOST_LEN + 16];
  int n = snprintf(line, sizeof line, "%s %u", mapping->external_host,
                   (unsigned)mapping->external_port);
  if (n > 0 && n < (int)sizeof line)
    push((Net *)mapping->user_ptr, TAG_MAP, line, n);
}

// -------------------------------------------------------- port mapping ---

// "a=candidate:1 1 UDP 2130706431 192.168.1.9 51234 typ host"
// Returns 1 for an IPv4 host candidate, filling ip and port.
int net_parse_host_candidate(const char *cand, char *ip, int ipcap, int *port) {
  const char *p = strstr(cand, "candidate:");
  if (!p)
    return 0;
  p += strlen("candidate:");

  char foundation[64], transport[16], addr[64], typ[16];
  unsigned prio;
  int component;
  if (sscanf(p, "%63s %d %15s %u %63s %d typ %15s", foundation, &component,
             transport, &prio, addr, port, typ) != 7)
    return 0;
  if (strcmp(typ, "host") || strchr(addr, ':')) // IPv6 needs no port mapping
    return 0;
  if (!strncmp(addr, "127.", 4))
    return 0;

  snprintf(ip, (size_t)ipcap, "%s", addr);
  return 1;
}

// Build the server-reflexive candidate the mapping just earned us, keeping the
// component and transport from the real host candidate it is derived from.
int net_mapped_candidate(const char *host_cand, const char *ext_ip, int ext_port,
                         char *out, int cap) {
  const char *p = strstr(host_cand, "candidate:");
  if (!p)
    return 0;
  p += strlen("candidate:");

  char foundation[64], transport[16], addr[64];
  unsigned prio;
  int component, port;
  if (sscanf(p, "%63s %d %15s %u %63s %d", foundation, &component, transport,
             &prio, addr, &port) != 6)
    return 0;

  // Distinct foundation, and the standard srflx priority so ICE still prefers
  // a genuine host pair when one exists.
  int n2 = snprintf(out, (size_t)cap,
                    "%scandidate:map%s %d %s 1694498815 %s %d typ srflx "
                    "raddr %s rport %d",
                    strncmp(host_cand, "a=", 2) ? "" : "a=", foundation,
                    component, transport, ext_ip, ext_port, addr, port);
  return n2 > 0 && n2 < cap;
}

// ------------------------------------------------------------------- open ---

int net_fd(const Net *n) { return n->pipe[0]; }

int net_alive(const Net *n) {
  int st = atomic_load((atomic_int *)&n->state);
  return st != JUICE_STATE_FAILED && st != JUICE_STATE_DISCONNECTED;
}

const char *net_route(const Net *n) {
  static char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  static char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  // "unknown" is not always permanent: the selected pair can be briefly
  // unavailable immediately after ICE reports COMPLETED. Callers that display
  // the route should re-read it rather than latching the first answer.
  if (juice_get_selected_candidates(n->agent, local, sizeof local, remote,
                                    sizeof remote) != 0)
    return "unknown";

  // Most specific first: a relayed pair is relayed however the other side is
  // described.
  if (strstr(local, "typ relay") || strstr(remote, "typ relay"))
    return "relay";
  if (strstr(local, "candidate:map") || strstr(remote, "candidate:map"))
    return "port-mapped";
  if (strstr(local, "typ srflx") || strstr(remote, "typ srflx"))
    return "srflx";
  // Peer-reflexive: an address learned from an incoming connectivity check
  // rather than from signaling, which happens whenever a candidate arrives
  // later than the packets sent from it. Still a direct path. Leaving this out
  // made a perfectly good direct connection report itself as "unknown".
  if (strstr(local, "typ prflx") || strstr(remote, "typ prflx"))
    return "prflx";
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

// Handle one signaling message. 0 means give up. *got_desc is set when the
// peer's description was applied.
static int on_signal_msg(Net *n, const char *msg, int *got_desc) {
  if (!strcmp(msg, "PEERGONE") || !strncmp(msg, "ERROR", 5)) {
    n->signaling_done = 1;
    snprintf(n->signaling_reason, sizeof n->signaling_reason, "%.*s",
             (int)sizeof n->signaling_reason - 1,
             msg[0] == 'P' ? "the other player left the signaling service"
                           : msg);
    vlog("signaling done: %s", n->signaling_reason);
    return 1; // carry on: ICE may well finish with the candidates we have
  }
  if (strncmp(msg, "SIGNAL ", 7))
    return 1; // unknown but harmless

  const char *payload = msg + 7;
  char tag = payload[0];
  const char *body = payload + 1;

  switch (tag) {
  case SIG_DESC:
    vlog("remote description received");
    if (juice_set_remote_description(n->agent, body) != 0) {
      set_err("the other player sent an unusable ICE description");
      return 0;
    }
    *got_desc = 1;
    return 1;
  case SIG_CAND:
    vlog("remote candidate: %s", body);
    juice_add_remote_candidate(n->agent, body); // a bad one is just ignored
    return 1;
  case SIG_DONE:
    vlog("remote gathering done");
    juice_set_remote_gathering_done(n->agent);
    return 1;
  default:
    return 1;
  }
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
    vlog("local candidate: %s", cand);
    send_signal(n, SIG_CAND, cand);

    // libjuice tells us the port it bound by publishing a host candidate, so
    // there is no need to pin one in advance -- which also means two clients
    // can share a machine.
    if (!n->mapping_asked &&
        net_parse_host_candidate(cand, n->host_ip, sizeof n->host_ip,
                                 &n->host_port)) {
      n->mapping_asked = 1;
      snprintf(n->host_cand, sizeof n->host_cand, "%s", cand);

      plum_mapping_t m = {.protocol = PLUM_IP_PROTOCOL_UDP,
                          .internal_port = (uint16_t)n->host_port,
                          .external_port = (uint16_t)n->host_port, // a hint
                          .user_ptr = n};
      n->mapping_id = plum_create_mapping(&m, on_mapping);
    }
    return 1;
  }
  case TAG_MAP: {
    char line[PLUM_MAX_HOST_LEN + 16];
    if (blen >= (int)sizeof line)
      return 1;
    memcpy(line, body, (size_t)blen);
    line[blen] = '\0';

    char ext_ip[PLUM_MAX_HOST_LEN];
    int ext_port;
    if (sscanf(line, "%255s %d", ext_ip, &ext_port) != 2)
      return 1;

    char cand[JUICE_MAX_CANDIDATE_SDP_STRING_LEN + 1];
    vlog("port mapping succeeded: %s", line);
    if (net_mapped_candidate(n->host_cand, ext_ip, ext_port, cand, (int)sizeof cand))
      send_signal(n, SIG_CAND, cand); // one more path for the peer to try
    return 1;
  }
  case TAG_DONE:
    vlog("local gathering done");
    send_signal(n, SIG_DONE, "");
    return 1;
  case TAG_DATA:
    if (connecting)
      stash(n, body, blen); // peer got there first; do not lose the message
    return 1;
  case TAG_STATE: {
    juice_state_t st = (juice_state_t)atomic_load(&n->state);
    vlog("ICE state: %s", juice_state_to_string(st));
    // Failure is reported, never worked around. If neither a direct path nor a
    // coturn allocation exists there is nowhere left to go.
    if (st == JUICE_STATE_FAILED) {
      set_err("ICE failed: no direct path and no working TURN relay");
      return 0;
    }
    return 1;
  }
  default:
    return 1;
  }
}

// Wrap one ICE payload in a SIGNAL message. The tag is ours; the service that
// forwards it neither reads nor understands it.
static int send_signal(Net *n, char tag, const char *body) {
  char msg[SIGNAL_MSG_MAX + 1];
  int len = snprintf(msg, sizeof msg, "SIGNAL %c%s", tag, body);
  if (len <= 0 || len >= (int)sizeof msg) {
    set_err("signaling payload too long");
    return 0;
  }
  return signal_send(n->sig, msg);
}

// Publish our ICE description and start gathering candidates.
static int offer(Net *n) {
  char local[JUICE_MAX_SDP_STRING_LEN];
  if (juice_get_local_description(n->agent, local, sizeof local) != 0) {
    set_err("could not build a local ICE description");
    return 0;
  }
  if (!send_signal(n, SIG_DESC, local)) {
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

  if (!signal_connect(&n->sig, cfg->signal_url, timeout)) {
    set_err(signal_error());
    goto fail;
  }

  if (cfg->room) {
    if (!signal_join_room(n->sig, cfg->room)) {
      set_err(signal_error());
      goto fail;
    }
    snprintf(room_out, (size_t)room_cap, "%s", cfg->room);
  } else {
    if (!signal_create_room(n->sig, room_out, room_cap, timeout)) {
      set_err(signal_error());
      goto fail;
    }
  }

  if (cfg->on_room)
    cfg->on_room(room_out, cfg->on_room_ctx);

  // The one place the program is idle on purpose: nothing can happen until a
  // human somewhere else types the code.
  if (!signal_wait_peer(n->sig, is_host, timeout)) {
    set_err(signal_error());
    goto fail;
  }
  vlog("paired as %s, room %s", *is_host ? "host" : "guest", room_out);

  // SOCK_DGRAM, not SOCK_STREAM: record boundaries must survive, otherwise two
  // datagrams written by the juice thread would arrive glued together.
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, n->pipe) != 0) {
    set_err(strerror(errno));
    goto fail;
  }
  // Non-blocking read end: net_recv() drains records in a loop and must be able
  // to discover "nothing left" without stalling the game.
  fcntl(n->pipe[0], F_SETFL, O_NONBLOCK);

  juice_turn_server_t turn[NET_MAX_TURN] = {0};
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
  int nturn = cfg->turn_count > NET_MAX_TURN ? NET_MAX_TURN : cfg->turn_count;
  for (int i = 0; i < nturn; i++) {
    turn[i].host = cfg->turn[i].host;
    turn[i].port = (unsigned short)cfg->turn[i].port;
    turn[i].username = cfg->turn[i].user;
    turn[i].password = cfg->turn[i].pass;
    // The password never appears here or anywhere else in this program.
    vlog("turn server %s:%d (user %s)", turn[i].host, turn[i].port,
         turn[i].username ? turn[i].username : "none");
  }
  if (!nturn)
    vlog("no turn server configured -- no relay fallback if direct paths fail");
  vlog("stun server: %s", cfg->stun_host ? cfg->stun_host : "none");
  if (nturn) {
    jc.turn_servers = turn;
    jc.turn_servers_count = nturn;
  }

  n->agent = juice_create(&jc);
  if (!n->agent) {
    set_err("juice_create failed");
    goto fail;
  }

  // Best effort. A router with none of PCP, NAT-PMP or UPnP just never calls
  // back, and ICE carries on exactly as it did before.
  n->mapping_id = -1;
  plum_config_t pc = {.log_level = PLUM_LOG_LEVEL_NONE};
  plum_init(&pc);

  // Whoever describes itself first becomes the ICE controlling agent. If both
  // do it, libjuice logs a role conflict and has to resolve it by tie-breaker.
  // The host offers; the guest answers only after applying the offer.
  int desc_sent = 0, remote_desc = 0;
  if (*is_host && !offer(n))
    goto fail;
  desc_sent = *is_host;

  // The wait for a human to type the code is over; from here we are only
  // waiting on machines, so a much shorter budget applies. Running out means
  // no path exists, which is a clean failure.
  long long deadline =
      now_ms() + (cfg->ice_timeout_ms > 0 ? cfg->ice_timeout_ms : 20000);
  for (;;) {
    if (atomic_load(&n->state) == JUICE_STATE_COMPLETED)
      break;
    if (now_ms() >= deadline) {
      if (n->signaling_done)
        set_err(n->signaling_reason);
      else
        set_err("timed out establishing a connection (no direct path, and no "
                "TURN relay reachable)");
      goto fail;
    }

    // The websocket is driven by libwebsockets rather than our poll loop, so
    // pump it briefly and then look at the ICE pipe. This lasts seconds, only
    // during setup, and ends before the game does any work.
    if (!n->signaling_done && signal_service(n->sig, 20) < 0) {
      n->signaling_done = 1;
      snprintf(n->signaling_reason, sizeof n->signaling_reason,
               "the signaling connection dropped during setup");
    }

    char msg[SIGNAL_MSG_MAX + 1];
    while (signal_next(n->sig, msg, sizeof msg)) {
      if (!on_signal_msg(n, msg, &remote_desc))
        goto fail;
      if (!desc_sent && remote_desc) {
        if (!offer(n))
          goto fail;
        desc_sent = 1;
      }
    }

    for (;;) {
      char rec[1 + NET_MAX_DATAGRAM];
      ssize_t r = read(n->pipe[0], rec, sizeof rec);
      if (r <= 0)
        break;
      if (!on_pipe_record(n, rec, (int)r, 1))
        goto fail;
    }
  }

  // ICE has a path, so signaling has done its entire job. Closing here is what
  // guarantees no game packet can ever travel through it.
  vlog("ICE completed, closing signaling");
  signal_close(n->sig);
  n->sig = NULL;

  *out = n;
  return 1;

fail:
  vlog("net_open failed: %s", last_err);
  if (n->mapping_id > 0)
    plum_destroy_mapping(n->mapping_id);
  if (n->agent) {
    plum_cleanup();
    juice_destroy(n->agent);
  }
  signal_close(n->sig);
  if (n->pipe[0] >= 0)
    close(n->pipe[0]);
  if (n->pipe[1] >= 0)
    close(n->pipe[1]);
  free(n);
  return 0;
}

// ------------------------------------------------------------------- data ---

int net_send(Net *n, const char *buf, int len) {
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

  // One source only. Signaling is closed by the time the game runs, so there
  // is no second path a game packet could arrive on.
  for (;;) {
    char rec[1 + NET_MAX_DATAGRAM];
    ssize_t r = read(n->pipe[0], rec, sizeof rec);
    if (r <= 0)
      return 0;
    if (rec[0] == TAG_DATA) {
      int len = (int)r - 1;
      if (len > cap)
        len = cap;
      memcpy(buf, rec + 1, (size_t)len);
      return len;
    }
    on_pipe_record(n, rec, (int)r, 0); // state changes and late candidates
  }
}

void net_close(Net *n) {
  if (!n)
    return;
  if (n->mapping_id >= 0)
    plum_destroy_mapping(n->mapping_id); // leaving a hole open would be rude
  plum_cleanup();
  if (n->agent)
    juice_destroy(n->agent); // joins the juice thread before the pipe closes
  signal_close(n->sig);      // normally already closed when ICE connected
  if (n->pipe[0] >= 0)
    close(n->pipe[0]);
  if (n->pipe[1] >= 0)
    close(n->pipe[1]);
  free(n);
}
