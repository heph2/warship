// Protocol tests. No sockets, no terminal, no real clock -- proto.c is pure, so
// packet loss, duplication and timeouts are just function calls.
#include "board.h"
#include "proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int enc(const Msg *m, char *buf) {
  return proto_encode(m, buf, PROTO_MAX_LINE);
}

static void test_roundtrip(void) {
  char buf[PROTO_MAX_LINE];
  Msg in, out;

  in = (Msg){.kind = MSG_FIRE, .seq = 7, .row = 3, .col = 9};
  int n = enc(&in, buf);
  assert(n > 0 && !strcmp(buf, "7 FIRE 3 9\n"));
  assert(proto_parse(buf, n, &out));
  assert(out.kind == MSG_FIRE && out.seq == 7 && out.row == 3 && out.col == 9);

  in = (Msg){.kind = MSG_RESULT, .seq = 8, .fire = FIRE_SUNK, .ship_idx = 4};
  n = enc(&in, buf);
  assert(proto_parse(buf, n, &out));
  assert(out.kind == MSG_RESULT && out.fire == FIRE_SUNK && out.ship_idx == 4);

  in = (Msg){.kind = MSG_HELLO, .seq = 0, .ver = PROTO_VERSION};
  strcpy(in.nick, "heph_1");
  n = enc(&in, buf);
  assert(proto_parse(buf, n, &out));
  assert(out.kind == MSG_HELLO && out.ver == PROTO_VERSION &&
         !strcmp(out.nick, "heph_1"));

  in = (Msg){.kind = MSG_ACK, .seq = 42};
  n = enc(&in, buf);
  assert(!strcmp(buf, "ACK 42\n"));
  assert(proto_parse(buf, n, &out) && out.kind == MSG_ACK && out.seq == 42);

  // A line with no trailing newline must still parse -- datagram boundaries,
  // not the newline, are what delimit a message.
  assert(proto_parse("3 READY", 7, &out) && out.kind == MSG_READY);
}

// Everything a hostile peer might send. All of it must be rejected, none of it
// may reach the game layer.
static void test_reject_garbage(void) {
  Msg out;
  const char *bad[] = {
      "",                     // empty
      "FIRE 1 1\n",           // no sequence number
      "1 FIRE 10 0\n",        // row off the board
      "1 FIRE 0 -1\n",        // negative column
      "1 FIRE 0\n",           // missing argument
      "1 RESULT 9 0\n",       // Fire enum out of range
      "1 RESULT 2 -1\n",      // claims SUNK but names no ship
      "1 RESULT 1 3\n",       // claims a ship on a plain HIT
      "1 RESULT 2 5\n",       // ship index past the fleet
      "1 HELLO 1 \x1b[2J\n",  // ANSI escape in the nick: would eat our screen
      "1 HELLO 1 bad nick\n", // space in the nick
      "1 HELLO 1 \n",         // empty nick
      "1 SHOOT 1 1\n",        // unknown verb
      "1 fire 1 1\n",         // verbs are case sensitive
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    assert(!proto_parse(bad[i], (int)strlen(bad[i]), &out));

  char nul[] = "1 REA\0DY";
  assert(!proto_parse(nul, 8, &out));

  char over[PROTO_MAX_LINE + 32];
  memset(over, 'x', sizeof over);
  assert(!proto_parse(over, (int)sizeof over, &out));

  // A nick one byte past the limit is refused rather than silently truncated.
  char longnick[128];
  int n = snprintf(longnick, sizeof longnick, "1 HELLO 1 %0*d\n", PROTO_NICK_MAX + 1, 0);
  assert(!proto_parse(longnick, n, &out));
}

static void test_ack_and_retransmit(void) {
  Proto p;
  proto_init(&p);
  assert(proto_timeout_ms(&p, 0) == -1); // idle: poll() may block forever

  Msg fire = {.kind = MSG_FIRE, .row = 1, .col = 1};
  assert(proto_send(&p, &fire, 0));
  assert(proto_in_flight(&p));

  const char *buf;
  int len;
  assert(proto_tick(&p, 0, &buf, &len)); // first transmission is immediate
  assert(!proto_tick(&p, 100, &buf, &len));
  assert(!proto_tick(&p, 499, &buf, &len));
  assert(proto_timeout_ms(&p, 100) == 400);
  assert(proto_tick(&p, 500, &buf, &len)); // retransmit is due

  char reply[PROTO_MAX_LINE];
  int reply_len;
  Msg got;
  char ack[PROTO_MAX_LINE];
  Msg ackmsg = {.kind = MSG_ACK, .seq = 0};
  int ack_len = enc(&ackmsg, ack);

  assert(!proto_recv(&p, ack, ack_len, 500, &got, reply, &reply_len));
  assert(!proto_in_flight(&p)); // acked, nothing left to send
  assert(!proto_tick(&p, 5000, &buf, &len));

  // A stale ACK for a sequence we are not holding must not pop anything.
  assert(proto_send(&p, &fire, 600));
  ackmsg.seq = 0;
  ack_len = enc(&ackmsg, ack);
  assert(!proto_recv(&p, ack, ack_len, 600, &got, reply, &reply_len));
  assert(proto_in_flight(&p));
}

static void test_peer_death(void) {
  Proto p;
  proto_init(&p);
  Msg m = {.kind = MSG_READY};
  proto_send(&p, &m, 0);

  const char *buf;
  int len;
  int sends = 0;
  for (long long t = 0; t <= PROTO_RETRANSMIT_MS * (PROTO_MAX_TRIES + 2); t += 100)
    if (proto_tick(&p, t, &buf, &len))
      sends++;

  assert(sends == PROTO_MAX_TRIES);
  assert(proto_dead(&p));
  assert(proto_timeout_ms(&p, 0) == -1); // a dead peer must not spin poll()
}

static void test_duplicate_delivery(void) {
  Proto p;
  proto_init(&p);
  char line[PROTO_MAX_LINE], reply[PROTO_MAX_LINE];
  int reply_len;
  Msg out;

  Msg fire = {.kind = MSG_FIRE, .seq = 0, .row = 2, .col = 2};
  int n = enc(&fire, line);

  assert(proto_recv(&p, line, n, 0, &out, reply, &reply_len) == 1);
  assert(reply_len > 0 && !strcmp(reply, "ACK 0\n"));

  // Same datagram again: acked again (our first ACK may have been lost) but
  // NOT handed to the game a second time.
  assert(proto_recv(&p, line, n, 0, &out, reply, &reply_len) == 0);
  assert(reply_len > 0 && !strcmp(reply, "ACK 0\n"));

  // Out of order: stop-and-wait means seq 2 cannot legitimately arrive yet.
  Msg ahead = {.kind = MSG_FIRE, .seq = 2, .row = 3, .col = 3};
  n = enc(&ahead, line);
  assert(proto_recv(&p, line, n, 0, &out, reply, &reply_len) == 0);

  Msg next = {.kind = MSG_FIRE, .seq = 1, .row = 4, .col = 4};
  n = enc(&next, line);
  assert(proto_recv(&p, line, n, 0, &out, reply, &reply_len) == 1);
  assert(out.row == 4 && out.col == 4);

  // Malformed input is never acked, so a broken peer eventually times out
  // instead of being told everything is fine.
  assert(proto_recv(&p, "junk", 4, 0, &out, reply, &reply_len) == 0);
  assert(reply_len == 0);
}

// ------------------------------------------------------- lossy end-to-end ---

typedef struct {
  Proto p;
  Board own;
  Track track;
  int next_shot;  // attacker: column to fire at next
  int awaiting;   // attacker: a FIRE is outstanding
} Peer;

static int drops;
static int dupes;
static int seq_counter;

// Every 3rd datagram is dropped, every 7th is delivered twice.
static int lossy(void) {
  seq_counter++;
  if (seq_counter % 3 == 0) {
    drops++;
    return 0;
  }
  return 1;
}
static int duplicated(void) {
  if (seq_counter % 7 == 0) {
    dupes++;
    return 1;
  }
  return 0;
}

static void on_message(Peer *self, Peer *peer, const Msg *m, long long now) {
  (void)peer;
  if (m->kind == MSG_FIRE) {
    int idx;
    Fire f = board_fire(&self->own, m->row, m->col, &idx);
    Msg r = {.kind = MSG_RESULT, .fire = f, .ship_idx = (f == FIRE_SUNK) ? idx : -1};
    assert(proto_send(&self->p, &r, now)); // outbox must never overflow here
  } else if (m->kind == MSG_RESULT) {
    track_mark(&self->track, 0, self->next_shot, m->fire, m->ship_idx);
    self->next_shot++;
    self->awaiting = 0;
  }
}

// Hand one datagram to `to`, then carry its ACK back -- both subject to loss.
static void deliver(Peer *to, Peer *from, const char *buf, int len, long long now) {
  char reply[PROTO_MAX_LINE];
  int reply_len;
  Msg m;

  int fresh = proto_recv(&to->p, buf, len, now, &m, reply, &reply_len);
  if (reply_len > 0 && lossy()) {
    Msg ignored;
    char r2[PROTO_MAX_LINE];
    int r2len;
    proto_recv(&from->p, reply, reply_len, now, &ignored, r2, &r2len);
    assert(r2len == 0); // an ACK must never provoke another ACK
  }
  if (fresh)
    on_message(to, from, &m, now);
}

static void pump(Peer *src, Peer *dst, long long now) {
  const char *buf;
  int len;
  if (!proto_tick(&src->p, now, &buf, &len))
    return;

  char copy[PROTO_MAX_LINE];
  memcpy(copy, buf, (size_t)len); // src's outbox may change during delivery

  if (!lossy())
    return;
  deliver(dst, src, copy, len, now);
  if (duplicated())
    deliver(dst, src, copy, len, now);
}

static void test_lossy_game(void) {
  Peer a = {0}, b = {0};
  proto_init(&a.p);
  proto_init(&b.p);

  board_place(&b.own, 0, 0, 0, 0); // Carrier, len 5, A1-E1
  const int SHOTS = 10;            // fires along row 0: 5 hits then 5 misses

  long long now = 0;
  for (; now < 600000 && a.next_shot < SHOTS; now += 50) {
    if (!a.awaiting && !proto_in_flight(&a.p) && a.next_shot < SHOTS) {
      Msg f = {.kind = MSG_FIRE, .row = 0, .col = a.next_shot};
      assert(proto_send(&a.p, &f, now));
      a.awaiting = 1;
    }
    pump(&a, &b, now);
    pump(&b, &a, now);
    assert(!proto_dead(&a.p) && !proto_dead(&b.p));
  }

  assert(a.next_shot == SHOTS);
  assert(drops > 0 && dupes > 0); // the channel really did misbehave

  // The whole point: duplicated FIRE datagrams must not double-count hits.
  assert(b.own.fleet[0].hits == 5);
  assert(board_all_sunk(&b.own) == 0);

  // Attacker's view agrees with the defender's truth, cell for cell.
  for (int c = 0; c < SHOTS; c++) {
    int ship = b.own.cell[0][c] != 0;
    TrackCell t = a.track.cell[0][c];
    assert(ship ? (t == TRACK_HIT || t == TRACK_SUNK) : (t == TRACK_MISS));
  }
  assert(a.track.cell[0][4] == TRACK_SUNK && a.track.sunk[0] == 1);
}

int main(void) {
  test_roundtrip();
  test_reject_garbage();
  test_ack_and_retransmit();
  test_peer_death();
  test_duplicate_delivery();
  test_lossy_game();
  printf("proto tests ok (%d dropped, %d duplicated)\n", drops, dupes);
  return 0;
}
