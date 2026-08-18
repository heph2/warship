#ifndef PROTO_H
#define PROTO_H

#include "board.h"

// Wire protocol + stop-and-wait reliability on top of unreliable datagrams.
//
// Pure state machine: no sockets, no clock, no globals. Every entry point takes
// `now_ms` from the caller so tests can drive it with a fake clock and a lossy
// channel. proto_now_ms() is provided for real callers and is the only function
// here that touches the outside world.
//
// Trust model: each side is authoritative over its own board and reports its own
// hits, so a modified client can lie. Accepted for a two-player game between
// people who chose each other. The real fix is commit-reveal (hash your layout at
// READY, reveal at game end, verify) -- deliberately out of scope.

#define PROTO_VERSION 1
#define PROTO_MAX_LINE 256
#define PROTO_RETRANSMIT_MS 500
#define PROTO_MAX_TRIES 20
#define PROTO_NICK_MAX 24
#define PROTO_QUEUE 4

typedef enum {
  MSG_NONE = 0,
  MSG_HELLO,  // <seq> HELLO <ver> <nick>
  MSG_READY,  // <seq> READY
  MSG_FIRE,   // <seq> FIRE <row> <col>
  MSG_RESULT, // <seq> RESULT <fire> <ship_idx>
  MSG_LOSE,   // <seq> LOSE
  MSG_BYE,    // <seq> BYE
  MSG_ACK     // ACK <seq>   -- transport only, never retransmitted
} MsgKind;

typedef struct {
  MsgKind kind;
  unsigned seq;
  int ver;                     // HELLO
  char nick[PROTO_NICK_MAX + 1]; // HELLO
  int row, col;                // FIRE
  Fire fire;                   // RESULT
  int ship_idx;                // RESULT
} Msg;

// One message is on the wire at a time, but the outbox is a short FIFO. Without
// it we deadlock: if our ACK is lost, the peer keeps retransmitting its reply
// while also expecting ours, and a single slot has nowhere to put the new
// message. Depth 4 is far more than strict alternation can ever need.
typedef struct {
  char buf[PROTO_MAX_LINE];
  int len;
  unsigned seq;
} OutMsg;

typedef struct {
  unsigned next_seq;
  OutMsg q[PROTO_QUEUE];
  int qhead, qcount;
  long long sent_at_ms; // -1 == head queued but not yet transmitted
  int tries;
  int have_peer_seq;
  unsigned last_peer_seq;
  int dead; // peer stopped acking
} Proto;

void proto_init(Proto *p);

// Queue one reliable message. Returns 0 only if the outbox is full or the peer
// is already declared dead.
int proto_send(Proto *p, const Msg *m, long long now_ms);

// Does the caller need to put bytes on the wire right now (first send or
// retransmit)? Returns 1 and points *buf/*len at them, and counts the attempt.
int proto_tick(Proto *p, long long now_ms, const char **buf, int *len);

// Feed one received datagram. Returns 1 when *out holds a NEW message for the
// game; 0 for an ACK, a duplicate, or anything malformed. When an ACK is owed,
// *reply/*reply_len is filled and the caller must send it.
int proto_recv(Proto *p, const char *data, int len, long long now_ms, Msg *out,
               char *reply, int *reply_len);

int proto_timeout_ms(const Proto *p, long long now_ms); // for poll(); -1 = block
int proto_in_flight(const Proto *p);
int proto_dead(const Proto *p);
int proto_tries(const Proto *p); // retransmits of the current message

// Encode without any state, for tests and for the ACK path.
int proto_encode(const Msg *m, char *buf, int cap);
int proto_parse(const char *data, int len, Msg *out);

long long proto_now_ms(void); // CLOCK_MONOTONIC

#endif
