#define _POSIX_C_SOURCE 200809L // clock_gettime under -std=c11

#include "proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

long long proto_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void proto_init(Proto *p) {
  memset(p, 0, sizeof *p);
  p->sent_at_ms = -1;
}

// ---------------------------------------------------------------- encode ---

int proto_encode(const Msg *m, char *buf, int cap) {
  int n;
  switch (m->kind) {
  case MSG_ACK:    n = snprintf(buf, (size_t)cap, "ACK %u\n", m->seq); break;
  case MSG_HELLO:  n = snprintf(buf, (size_t)cap, "%u HELLO %d %s\n", m->seq, m->ver, m->nick); break;
  case MSG_READY:  n = snprintf(buf, (size_t)cap, "%u READY\n", m->seq); break;
  case MSG_FIRE:   n = snprintf(buf, (size_t)cap, "%u FIRE %d %d\n", m->seq, m->row, m->col); break;
  case MSG_RESULT: n = snprintf(buf, (size_t)cap, "%u RESULT %d %d\n", m->seq, (int)m->fire, m->ship_idx); break;
  case MSG_LOSE:   n = snprintf(buf, (size_t)cap, "%u LOSE\n", m->seq); break;
  case MSG_BYE:    n = snprintf(buf, (size_t)cap, "%u BYE\n", m->seq); break;
  default: return 0;
  }
  return (n > 0 && n < cap) ? n : 0; // refuse to emit a truncated line
}

// ----------------------------------------------------------------- parse ---

// Everything below this line is a trust boundary: the bytes came from a peer we
// do not control. Validate ranges, and never let unprintable bytes through --
// a nick containing an ANSI escape would rewrite our terminal when displayed.
static int nick_ok(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > PROTO_NICK_MAX)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_')
      return 0;
  return 1;
}

int proto_parse(const char *data, int len, Msg *out) {
  if (len <= 0 || len > PROTO_MAX_LINE)
    return 0;

  char line[PROTO_MAX_LINE + 1];
  memcpy(line, data, (size_t)len);
  line[len] = '\0';

  if (len > 0 && line[len - 1] == '\n')
    line[--len] = '\0';
  if ((int)strlen(line) != len)
    return 0; // embedded NUL
  for (int i = 0; i < len; i++)
    if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] >= 0x7f)
      return 0;

  memset(out, 0, sizeof *out);

  // `used` guards against trailing junk: sscanf happily ignores anything after
  // the last conversion, so "HELLO 1 bad nick" would otherwise parse as the
  // nick "bad". Every branch must consume the entire line.
  int used = -1;
  unsigned seq;
  if (sscanf(line, "ACK %u%n", &seq, &used) == 1) {
    if (used != len)
      return 0;
    out->kind = MSG_ACK;
    out->seq = seq;
    return 1;
  }

  char verb[16];
  int after_verb = -1;
  if (sscanf(line, "%u %15s%n", &seq, verb, &after_verb) != 2)
    return 0;
  out->seq = seq;

  if (!strcmp(verb, "HELLO")) {
    char nick[PROTO_NICK_MAX + 2];
    if (sscanf(line, "%*u %*15s %d %25s%n", &out->ver, nick, &used) != 2)
      return 0;
    if (used != len || !nick_ok(nick))
      return 0;
    memcpy(out->nick, nick, strlen(nick) + 1);
    out->kind = MSG_HELLO;
  } else if (!strcmp(verb, "FIRE")) {
    if (sscanf(line, "%*u %*15s %d %d%n", &out->row, &out->col, &used) != 2)
      return 0;
    if (used != len)
      return 0;
    if (out->row < 0 || out->row >= SIZE || out->col < 0 || out->col >= SIZE)
      return 0;
    out->kind = MSG_FIRE;
  } else if (!strcmp(verb, "RESULT")) {
    int fire;
    if (sscanf(line, "%*u %*15s %d %d%n", &fire, &out->ship_idx, &used) != 2)
      return 0;
    if (used != len)
      return 0;
    if (fire < FIRE_REJECT || fire > FIRE_SUNK)
      return 0;
    if (out->ship_idx < -1 || out->ship_idx >= NSHIPS)
      return 0;
    // A sunk ship must say which one; anything else must not claim one.
    if ((fire == FIRE_SUNK) != (out->ship_idx >= 0))
      return 0;
    out->fire = (Fire)fire;
    out->kind = MSG_RESULT;
  } else if (!strcmp(verb, "READY") || !strcmp(verb, "LOSE") ||
             !strcmp(verb, "BYE")) {
    if (after_verb != len)
      return 0; // argument-less verbs take no arguments
    out->kind = verb[0] == 'R' ? MSG_READY : verb[0] == 'L' ? MSG_LOSE : MSG_BYE;
  } else {
    return 0; // unknown verb: a newer peer, or garbage. Same treatment.
  }
  return 1;
}

// ------------------------------------------------------------ reliability ---

int proto_in_flight(const Proto *p) { return p->qcount > 0; }
int proto_dead(const Proto *p) { return p->dead; }
int proto_tries(const Proto *p) { return p->tries; }

static OutMsg *head(Proto *p) { return &p->q[p->qhead]; }

static void pop(Proto *p) {
  p->qhead = (p->qhead + 1) % PROTO_QUEUE;
  p->qcount--;
  p->sent_at_ms = -1;
  p->tries = 0;
}

int proto_send(Proto *p, const Msg *m, long long now_ms) {
  (void)now_ms;
  if (p->dead || p->qcount == PROTO_QUEUE)
    return 0;

  Msg copy = *m;
  copy.seq = p->next_seq;

  OutMsg *slot = &p->q[(p->qhead + p->qcount) % PROTO_QUEUE];
  int n = proto_encode(&copy, slot->buf, sizeof slot->buf);
  if (!n)
    return 0;
  slot->len = n;
  slot->seq = copy.seq;

  p->qcount++;
  p->next_seq++;
  return 1;
}

int proto_tick(Proto *p, long long now_ms, const char **buf, int *len) {
  if (!p->qcount || p->dead)
    return 0;
  if (p->sent_at_ms >= 0 && now_ms - p->sent_at_ms < PROTO_RETRANSMIT_MS)
    return 0;

  if (++p->tries > PROTO_MAX_TRIES) {
    p->dead = 1;
    return 0;
  }
  p->sent_at_ms = now_ms;
  *buf = head(p)->buf;
  *len = head(p)->len;
  return 1;
}

int proto_timeout_ms(const Proto *p, long long now_ms) {
  if (!p->qcount || p->dead)
    return -1; // nothing pending: block until a key or a datagram arrives
  if (p->sent_at_ms < 0)
    return 0;
  long long due = p->sent_at_ms + PROTO_RETRANSMIT_MS - now_ms;
  return due < 0 ? 0 : (int)due;
}

int proto_recv(Proto *p, const char *data, int len, long long now_ms, Msg *out,
               char *reply, int *reply_len) {
  *reply_len = 0;

  Msg m;
  if (!proto_parse(data, len, &m))
    return 0;

  if (m.kind == MSG_ACK) {
    // Only the head can be outstanding; an ACK for anything else is stale.
    if (p->qcount && m.seq == head(p)->seq)
      pop(p);
    return 0; // ACKs are never retransmitted and never reach the game
  }

  // Owe an ACK for anything well-formed, including duplicates -- the peer is
  // retransmitting precisely because our last ACK went missing.
  Msg ack = {.kind = MSG_ACK, .seq = m.seq};
  *reply_len = proto_encode(&ack, reply, PROTO_MAX_LINE);

  // Stop-and-wait: the peer cannot advance until we ack, so the only sequence
  // we will ever accept is the next one. Everything else is a duplicate.
  unsigned expect = p->have_peer_seq ? p->last_peer_seq + 1 : 0;
  if (m.seq != expect)
    return 0;

  p->have_peer_seq = 1;
  p->last_peer_seq = m.seq;
  (void)now_ms;
  *out = m;
  return 1;
}
