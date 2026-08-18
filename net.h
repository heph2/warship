#ifndef NET_H
#define NET_H

// ICE transport. Wraps libjuice and the rendezvous handshake so the rest of the
// program sees one pollable fd and two functions.
//
// libjuice invokes its callbacks from its own thread. Nothing in this module
// lets that thread touch game state or the signaling socket: every callback does
// exactly one thing, write a tagged record into a socketpair. The main loop owns
// everything else, so there is not a single mutex in the program.
//
// What comes out is still UNRELIABLE datagrams -- ICE is not DTLS and not SCTP.
// Loss, duplication and reordering are proto.c's problem, by design.

#include <poll.h>

#define NET_MAX_DATAGRAM 1200 // comfortably under any real path MTU
#define NET_MAX_POLLFDS 2

typedef struct Net Net;

typedef struct {
  const char *signal_host;
  const char *signal_port;
  const char *room; // NULL: create a room and report the code back
  const char *stun_host;
  int stun_port;
  const char *turn_host; // NULL: no relay fallback (symmetric NAT will fail)
  int turn_port;
  const char *turn_user;
  const char *turn_pass;
  int timeout_ms;     // how long to wait for a human to type the code
  // How long to attempt a direct path before falling back to the relay.
  // Negative skips ICE entirely and relays from the start, which is the only
  // deterministic way to exercise the relay: on loopback a direct connection
  // can complete in about a millisecond, so no timeout is reliably shorter.
  int ice_timeout_ms;

  // Called as soon as the room code is known, which is BEFORE the wait for the
  // other player. Without this the host could never show the code it is asking
  // someone to type.
  void (*on_room)(const char *code, void *ctx);
  void *on_room_ctx;
} NetConfig;

// Runs the whole handshake and blocks until ICE connects. On success *is_host
// says who fires first, and room_out holds the code (useful when cfg->room was
// NULL and the server invented one).
int net_open(Net **out, const NetConfig *cfg, char *room_out, int room_cap,
             int *is_host);

// Fill in the descriptors the caller must poll. Returns how many were written.
// There is more than one: the ICE datagram pipe, and the signaling socket,
// which now stays open for the whole game so it can relay if ICE cannot find a
// direct path.
int net_pollfds(const Net *n, struct pollfd *out, int cap);
int net_send(Net *n, const char *buf, int len);

// One datagram, or 0 when nothing is pending. Never blocks.
int net_recv(Net *n, char *buf, int cap);

int net_alive(const Net *n); // 0 once neither ICE nor the relay can carry data

// "host", "port-mapped" (the router opened a port for us), "srflx", "relay"
// (TURN) or "signal-relay" (through the rendezvous server, the fallback that
// needs no TURN deployment at all).
const char *net_route(const Net *n);
void net_close(Net *n);
const char *net_error(void);

// Candidate rewriting, exposed because it is pure string handling and the one
// part of the mapping path that can be tested without a router.
int net_parse_host_candidate(const char *cand, char *ip, int ipcap, int *port);
int net_mapped_candidate(const char *host_cand, const char *ext_ip, int ext_port,
                         char *out, int cap);

#endif
