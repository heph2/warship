#ifndef NET_H
#define NET_H

// ICE transport. Wraps libjuice and the signaling handshake so the rest of the
// program sees one pollable fd and two functions.
//
// libjuice and libplum invoke their callbacks from their own threads. Nothing
// here lets those threads touch game state: every callback does exactly one
// thing, write a tagged record into a socketpair. The main loop owns
// everything else, so there is not a single mutex in the program.
//
// Game traffic only ever travels over ICE: a direct path, a port the router
// opened for us, or a coturn relay candidate. The signaling websocket is
// closed the moment ICE connects and never carries a game packet.
//
// What comes out is still UNRELIABLE datagrams -- ICE is not DTLS and not
// SCTP. Loss, duplication and reordering are proto.c's problem, by design.

#define NET_MAX_DATAGRAM 1200 // comfortably under any real path MTU
#define NET_MAX_TURN 4

typedef struct Net Net;

typedef struct {
  const char *host;
  int port;
  const char *user;
  const char *pass;
} NetTurn;

typedef struct {
  const char *signal_url; // ws://host[:port][/path] or wss://...
  const char *room;       // NULL: create a room and report the code back

  const char *stun_host; // NULL disables STUN entirely
  int stun_port;

  // libjuice takes TURN over UDP only: its juice_turn_server_t has no
  // transport field. turns: on 5349 cannot be configured, so do not offer it.
  NetTurn turn[NET_MAX_TURN];
  int turn_count;

  int timeout_ms;     // how long to wait for a human to type the code
  int ice_timeout_ms; // how long to attempt connectivity before giving up

  // Called as soon as the room code is known, which is BEFORE the wait for the
  // other player. Without this the host could never show the code it is asking
  // someone to type.
  void (*on_room)(const char *code, void *ctx);
  void *on_room_ctx;
} NetConfig;

// Runs the whole handshake and blocks until ICE connects. Failure to find any
// path -- direct, mapped or relayed -- is a clean error, not a fallback.
int net_open(Net **out, const NetConfig *cfg, char *room_out, int room_cap,
             int *is_host);

int net_fd(const Net *n); // poll() this for readability
int net_send(Net *n, const char *buf, int len);

// One datagram, or 0 when nothing is pending. Never blocks.
int net_recv(Net *n, char *buf, int cap);

int net_alive(const Net *n);

// "host", "port-mapped" (the router opened a port for us), "srflx" or "relay"
// (a coturn allocation).
const char *net_route(const Net *n);
void net_close(Net *n);
const char *net_error(void);

// Candidate rewriting, exposed because it is pure string handling and the one
// part of the mapping path that can be tested without a router.
int net_parse_host_candidate(const char *cand, char *ip, int ipcap, int *port);
int net_mapped_candidate(const char *host_cand, const char *ext_ip, int ext_port,
                         char *out, int cap);

#endif
