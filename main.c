#define _POSIX_C_SOURCE 200809L

#include "board.h"
#include "net.h"
#include "proto.h"
#include "ui.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_SIGNAL_PORT "7777"
#define DEFAULT_STUN_HOST "stun.l.google.com"
#define DEFAULT_STUN_PORT 19302

typedef enum {
  ST_WAIT_READY, // both sides placed; waiting for the peer to say so
  ST_MY_TURN,
  ST_WAIT_RESULT, // our shot is out there
  ST_THEIR_TURN,
  ST_OVER
} State;

typedef struct {
  Board own;
  Track track;
  Proto proto;
  Net *net;
  State st;
  int row, col;    // cursor over enemy waters
  int shot_row, shot_col; // the shot we are waiting on
  int is_host;
  int peer_ready;
  char status[160];
} Game;

// ------------------------------------------------------------- placement ---

// Returns 0 if the player quit before the fleet was ready.
static int placement_loop(Board *b) {
  int row = 0, col = 0, vert = 0;

  for (;;) {
    int len = (b->placed < NSHIPS) ? SHIP_LEN[b->placed] : 1;
    int max_row = vert ? SIZE - len : SIZE - 1;
    int max_col = vert ? SIZE - 1 : SIZE - len;
    if (row > max_row) row = max_row; // re-clamp after a rotate or a commit
    if (col > max_col) col = max_col;

    ui_draw_placement(b, row, col, vert);

    switch (ui_key()) {
    case 'w': if (row > 0)       row--; break;
    case 's': if (row < max_row) row++; break;
    case 'a': if (col > 0)       col--; break;
    case 'd': if (col < max_col) col++; break;
    case 'r': vert = !vert; break;
    case ' ':
      if (b->placed < NSHIPS && board_can_place(b, row, col, len, vert))
        board_place(b, b->placed++, row, col, vert);
      break;
    case 'u':
      if (b->placed > 0) {
        board_unplace(b, --b->placed);
        row = b->fleet[b->placed].row;
        col = b->fleet[b->placed].col;
        vert = b->fleet[b->placed].vertical;
      }
      break;
    case '\r':
    case '\n':
      if (b->placed == NSHIPS)
        return 1;
      break;
    case 'q':
    case -1:
      return 0;
    }
  }
}

// ----------------------------------------------------------------- combat ---

static void say(Game *g, const char *s) {
  snprintf(g->status, sizeof g->status, "%.*s", (int)sizeof g->status - 1, s);
}

static void queue(Game *g, const Msg *m) {
  if (!proto_send(&g->proto, m, proto_now_ms()))
    say(g, "outbox full -- connection is struggling");
}

static void on_fire(Game *g, const Msg *m) {
  int idx;
  Fire f = board_fire(&g->own, m->row, m->col, &idx);

  Msg r = {.kind = MSG_RESULT, .fire = f, .ship_idx = (f == FIRE_SUNK) ? idx : -1};
  queue(g, &r);

  char note[128];
  switch (f) {
  case FIRE_REJECT: return; // duplicate square: no turn changes hands
  case FIRE_MISS: snprintf(note, sizeof note, "they fired at %c%d -- miss",
                           'A' + m->col, m->row + 1); break;
  case FIRE_HIT:  snprintf(note, sizeof note, "they hit your %s at %c%d",
                           SHIP_NAME[idx], 'A' + m->col, m->row + 1); break;
  case FIRE_SUNK: snprintf(note, sizeof note, "they SANK your %s", SHIP_NAME[idx]);
                  break;
  }

  if (board_all_sunk(&g->own)) {
    Msg lose = {.kind = MSG_LOSE};
    queue(g, &lose);
    say(g, "your fleet is gone -- you lose. q to quit");
    g->st = ST_OVER;
    return;
  }
  say(g, note);
  g->st = ST_MY_TURN;
}

static void on_result(Game *g, const Msg *m) {
  if (m->fire == FIRE_REJECT) {
    say(g, "they say you already fired there -- pick another square");
    g->st = ST_MY_TURN;
    return;
  }

  track_mark(&g->track, g->shot_row, g->shot_col, m->fire, m->ship_idx);

  int won = 1;
  for (int i = 0; i < NSHIPS; i++)
    if (!g->track.sunk[i])
      won = 0;

  char note[128];
  switch (m->fire) {
  case FIRE_MISS: snprintf(note, sizeof note, "miss at %c%d",
                           'A' + g->shot_col, g->shot_row + 1); break;
  case FIRE_HIT:  snprintf(note, sizeof note, "hit at %c%d!",
                           'A' + g->shot_col, g->shot_row + 1); break;
  case FIRE_SUNK: snprintf(note, sizeof note, "you SANK their %s!",
                           SHIP_NAME[m->ship_idx]); break;
  default: note[0] = '\0'; break;
  }

  if (won) {
    say(g, "every enemy ship is down -- you win. q to quit");
    g->st = ST_OVER;
    return;
  }
  say(g, note);
  g->st = ST_THEIR_TURN;
}

static void on_message(Game *g, const Msg *m) {
  switch (m->kind) {
  case MSG_HELLO:
    if (m->ver != PROTO_VERSION) {
      say(g, "peer speaks a different protocol version -- q to quit");
      g->st = ST_OVER;
    }
    break;
  case MSG_READY:
    g->peer_ready = 1;
    break;
  case MSG_FIRE:
    if (g->st == ST_THEIR_TURN)
      on_fire(g, m);
    break;
  case MSG_RESULT:
    if (g->st == ST_WAIT_RESULT)
      on_result(g, m);
    break;
  case MSG_LOSE:
    say(g, "they concede -- you win. q to quit");
    g->st = ST_OVER;
    break;
  case MSG_BYE:
    say(g, "peer left the game -- q to quit");
    g->st = ST_OVER;
    break;
  default:
    break;
  }
}

static void on_key(Game *g, int key) {
  switch (key) {
  case 'w': if (g->row > 0)        g->row--; break;
  case 's': if (g->row < SIZE - 1) g->row++; break;
  case 'a': if (g->col > 0)        g->col--; break;
  case 'd': if (g->col < SIZE - 1) g->col++; break;
  case ' ':
    if (g->st != ST_MY_TURN)
      break;
    // Refuse locally rather than burning a round trip to be told no.
    if (g->track.cell[g->row][g->col] != TRACK_UNKNOWN) {
      say(g, "you already fired there");
      break;
    }
    g->shot_row = g->row;
    g->shot_col = g->col;
    Msg f = {.kind = MSG_FIRE, .row = g->row, .col = g->col};
    queue(g, &f);
    say(g, "shot away...");
    g->st = ST_WAIT_RESULT;
    break;
  }
}

// The status line has to distinguish "blocked on the peer's move" from
// "blocked on the network", or a stalled game just looks frozen.
static void draw(Game *g) {
  char line[256];
  const char *phase = "";
  switch (g->st) {
  case ST_WAIT_READY:  phase = "waiting for the other fleet..."; break;
  case ST_MY_TURN:     phase = "your turn"; break;
  case ST_WAIT_RESULT: phase = "waiting for the result..."; break;
  case ST_THEIR_TURN:  phase = "their turn..."; break;
  case ST_OVER:        phase = ""; break;
  }

  int tries = proto_tries(&g->proto);
  if (tries > 2 && g->st != ST_OVER)
    snprintf(line, sizeof line, "%s   [reconnecting, attempt %d/%d]",
             g->status[0] ? g->status : phase, tries, PROTO_MAX_TRIES);
  else if (g->status[0] && g->st != ST_WAIT_READY)
    snprintf(line, sizeof line, "%s   %s", g->status, phase);
  else
    snprintf(line, sizeof line, "%s", phase);

  ui_draw_battle(&g->own, &g->track, g->row, g->col, line);
}

static void battle_loop(Game *g) {
  char reply[PROTO_MAX_LINE];
  int reply_len;

  for (;;) {
    long long now = proto_now_ms();

    const char *out;
    int out_len;
    if (proto_tick(&g->proto, now, &out, &out_len))
      net_send(g->net, out, out_len);

    if (proto_dead(&g->proto)) {
      say(g, "lost contact with the peer -- q to quit");
      g->st = ST_OVER;
    }
    if (!net_alive(g->net) && g->st != ST_OVER) {
      say(g, "the connection dropped -- q to quit");
      g->st = ST_OVER;
    }

    draw(g);

    int timeout = proto_timeout_ms(&g->proto, now);
    struct pollfd fds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = net_fd(g->net), .events = POLLIN},
    };
    if (poll(fds, 2, timeout) < 0) {
      if (errno == EINTR)
        continue;
      return;
    }

    if (fds[0].revents & POLLIN) {
      int key = ui_key();
      if (key == 'q' || key == -1)
        break;
      on_key(g, key);
    }

    if (fds[1].revents & POLLIN) {
      char buf[NET_MAX_DATAGRAM];
      int len;
      while ((len = net_recv(g->net, buf, sizeof buf)) > 0) {
        Msg m;
        if (proto_recv(&g->proto, buf, len, now, &m, reply, &reply_len))
          on_message(g, &m);
        if (reply_len > 0)
          net_send(g->net, reply, reply_len);
      }
    }

    // READY is what proves the peer's game loop is actually running, not just
    // that ICE connected. Firing before it would shoot into a void.
    if (g->st == ST_WAIT_READY && g->peer_ready)
      g->st = g->is_host ? ST_MY_TURN : ST_THEIR_TURN;
  }

  Msg bye = {.kind = MSG_BYE};
  if (proto_send(&g->proto, &bye, proto_now_ms())) {
    const char *out;
    int out_len;
    if (proto_tick(&g->proto, proto_now_ms(), &out, &out_len))
      net_send(g->net, out, out_len); // best effort; we are leaving either way
  }
}

// -------------------------------------------------------------- plumbing ---

static char room_code[16];

// `code` already points at room_code -- copying it onto itself with snprintf is
// an overlapping copy, which is undefined and in practice yields an empty
// string. Just render it.
static void show_room(const char *code, void *ctx) {
  (void)ctx;
  char line[128];
  snprintf(line, sizeof line, "room code:  %s", code);
  ui_message(line, "tell your opponent to run:  warship join <code>");
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s host [options]\n"
          "  %s join <code> [options]\n"
          "\n"
          "options:\n"
          "  --signal HOST[:PORT]   rendezvous server (default port %s)\n"
          "  --stun HOST[:PORT]     STUN server (default %s:%d)\n"
          "  --turn HOST[:PORT]     TURN relay, needed behind symmetric NAT\n"
          "  --turn-user USER\n"
          "  --turn-pass PASS\n"
          "  --nick NAME\n",
          argv0, argv0, DEFAULT_SIGNAL_PORT, DEFAULT_STUN_HOST, DEFAULT_STUN_PORT);
}

// Split "host:port" in place. Returns the port, or `fallback` if none given.
static const char *split_port(char *hostport, const char *fallback) {
  char *colon = strrchr(hostport, ':');
  if (!colon || strchr(hostport, ':') != colon) // leave bare IPv6 alone
    return fallback;
  *colon = '\0';
  return colon + 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  int is_join = !strcmp(argv[1], "join");
  if (!is_join && strcmp(argv[1], "host")) {
    usage(argv[0]);
    return 1;
  }

  int i = 2;
  const char *room = NULL;
  if (is_join) {
    if (argc < 3) {
      usage(argv[0]);
      return 1;
    }
    room = argv[i++];
  }

  char signal_arg[256] = "127.0.0.1";
  char stun_arg[256] = DEFAULT_STUN_HOST;
  char turn_arg[256] = "";
  const char *stun_port_s = NULL, *turn_port_s = "3478";
  const char *turn_user = NULL, *turn_pass = NULL;
  const char *nick = "player";

  for (; i < argc; i++) {
    const char *a = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(a, "--signal") && val) snprintf(signal_arg, sizeof signal_arg, "%s", argv[++i]);
    else if (!strcmp(a, "--stun") && val) snprintf(stun_arg, sizeof stun_arg, "%s", argv[++i]);
    else if (!strcmp(a, "--turn") && val) snprintf(turn_arg, sizeof turn_arg, "%s", argv[++i]);
    else if (!strcmp(a, "--turn-user") && val) turn_user = argv[++i];
    else if (!strcmp(a, "--turn-pass") && val) turn_pass = argv[++i];
    else if (!strcmp(a, "--nick") && val) nick = argv[++i];
    else {
      usage(argv[0]);
      return 1;
    }
  }

  const char *signal_port = split_port(signal_arg, DEFAULT_SIGNAL_PORT);
  stun_port_s = split_port(stun_arg, NULL);
  turn_port_s = turn_arg[0] ? split_port(turn_arg, "3478") : NULL;

  Game *g = calloc(1, sizeof *g);
  if (!g)
    return 1;

  ui_init();

  // Place first, connect second. If we connected first, the peer's READY would
  // start retransmitting while we were still choosing where the carrier goes,
  // and its 10-second budget would expire before we finished.
  if (!placement_loop(&g->own)) {
    free(g);
    return 0;
  }
  ui_message("connecting...", NULL);

  NetConfig cfg = {
      .signal_host = signal_arg,
      .signal_port = signal_port,
      .room = room,
      .stun_host = stun_arg,
      .stun_port = stun_port_s ? atoi(stun_port_s) : DEFAULT_STUN_PORT,
      .turn_host = turn_arg[0] ? turn_arg : NULL,
      .turn_port = turn_port_s ? atoi(turn_port_s) : 0,
      .turn_user = turn_user,
      .turn_pass = turn_pass,
      .timeout_ms = 300000, // five minutes for a human to type the code
      .on_room = show_room,
  };

  int is_host = 0;
  if (!net_open(&g->net, &cfg, room_code, sizeof room_code, &is_host)) {
    ui_message("could not connect:", net_error());
    ui_key();
    free(g);
    return 1;
  }

  proto_init(&g->proto);
  Msg hello = {.kind = MSG_HELLO, .ver = PROTO_VERSION};
  snprintf(hello.nick, sizeof hello.nick, "%.*s", PROTO_NICK_MAX, nick);
  queue(g, &hello);
  Msg ready = {.kind = MSG_READY};
  queue(g, &ready);

  // The host offered the ICE session, so the host shoots first -- but only
  // once the peer has said READY.
  g->is_host = is_host;
  g->st = ST_WAIT_READY;
  snprintf(g->status, sizeof g->status, "connected over %s", net_route(g->net));

  battle_loop(g);

  net_close(g->net);
  free(g);
  return 0;
}
