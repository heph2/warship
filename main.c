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

// Baked in so that `warship host` needs no arguments at all. Every one of
// these can still be overridden, and "none" disables STUN or TURN outright.
// *.pochi.casa is IPv6-only, which is also why NAT traversal matters less
// here than it would on v4: the hosts are globally addressable and the
// question is firewalls, not translation.
#define DEFAULT_SIGNAL_URL "wss://signal.pochi.casa/"
#define DEFAULT_STUN_SERVER "turn.pochi.casa:3478"
#define DEFAULT_TURN_SERVER "turn.pochi.casa:3478"
#define DEFAULT_TURN_USER "warship"
#define DEFAULT_TURN_PORT 3478

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
  const char *route; // last reported path, so a fallback can be announced
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
    case 'w': case UI_KEY_UP:    if (row > 0)       row--; break;
    case 's': case UI_KEY_DOWN:  if (row < max_row) row++; break;
    case 'a': case UI_KEY_LEFT:  if (col > 0)       col--; break;
    case 'd': case UI_KEY_RIGHT: if (col < max_col) col++; break;
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
  case 'w': case UI_KEY_UP:    if (g->row > 0)        g->row--; break;
  case 's': case UI_KEY_DOWN:  if (g->row < SIZE - 1) g->row++; break;
  case 'a': case UI_KEY_LEFT:  if (g->col > 0)        g->col--; break;
  case 'd': case UI_KEY_RIGHT: if (g->col < SIZE - 1) g->col++; break;
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

    // Falling back to the relay mid-game must be visible. Silence here looks
    // identical to a hang.
    const char *route = net_route(g->net);
    if (route != g->route && strcmp(route, g->route)) {
      char note[96];
      snprintf(note, sizeof note, "connection is now via %s", route);
      say(g, note);
      g->route = route;
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

    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
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
          "With no options at all this uses the pochi.casa deployment:\n"
          "  signal   %s\n"
          "  stun     %s\n"
          "  turn     %s (user %s)\n"
          "\n"
          "options:\n"
          "  --signal-url URL       ws:// or wss:// signaling service\n"
          "  --stun-server H[:P]    or \"none\"\n"
          "  --turn-server H[:P]    repeatable, or \"none\"; default port %d\n"
          "  --turn-user USER       or $WARSHIP_TURN_USER\n"
          "  --turn-password PASS   or $WARSHIP_TURN_PASSWORD\n"
          "  --nick NAME\n"
          "\n"
          "TURN needs a password and is skipped without one, which only costs\n"
          "you the relay of last resort. Read it from the environment rather\n"
          "than a flag to keep it out of your shell history and out of ps.\n"
          "libjuice speaks TURN over UDP only; turns:// on 5349 is not usable.\n",
          argv0, argv0, DEFAULT_SIGNAL_URL, DEFAULT_STUN_SERVER,
          DEFAULT_TURN_SERVER, DEFAULT_TURN_USER, DEFAULT_TURN_PORT);
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

  static char stun_arg[256] = DEFAULT_STUN_SERVER;
  static char turn_arg[NET_MAX_TURN][256];
  const char *signal_url = DEFAULT_SIGNAL_URL;
  int turn_count = 0;
  int turn_explicit = 0; // a --turn-server was given, so stop defaulting
  const char *turn_user = getenv("WARSHIP_TURN_USER");
  const char *turn_pass = getenv("WARSHIP_TURN_PASSWORD");
  const char *nick = "player";

  for (; i < argc; i++) {
    const char *a = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(a, "--signal-url") && val) {
      signal_url = argv[++i];
    } else if (!strcmp(a, "--stun-server") && val) {
      snprintf(stun_arg, sizeof stun_arg, "%s", argv[++i]);
      if (!strcmp(stun_arg, "none"))
        stun_arg[0] = '\0';
    } else if (!strcmp(a, "--turn-server") && val) {
      turn_explicit = 1;
      if (!strcmp(argv[i + 1], "none")) {
        i++;
        turn_count = 0;
        continue;
      }
      if (turn_count >= NET_MAX_TURN) {
        fprintf(stderr, "at most %d TURN servers\n", NET_MAX_TURN);
        return 1;
      }
      snprintf(turn_arg[turn_count++], sizeof turn_arg[0], "%s", argv[++i]);
    } else if (!strcmp(a, "--turn-user") && val) {
      turn_user = argv[++i];
    } else if (!strcmp(a, "--turn-password") && val) {
      turn_pass = argv[++i];
    } else if (!strcmp(a, "--nick") && val) {
      nick = argv[++i];
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (!turn_user)
    turn_user = DEFAULT_TURN_USER;

  // The default relay is only worth configuring if we can authenticate to it.
  // Without a password we simply go without, which costs the fallback and
  // nothing else.
  if (!turn_explicit && turn_pass)
    snprintf(turn_arg[turn_count++], sizeof turn_arg[0], "%s",
             DEFAULT_TURN_SERVER);

  if (turn_count && !turn_pass) {
    fprintf(stderr, "a TURN server needs a password: --turn-password or "
                    "WARSHIP_TURN_PASSWORD\n");
    return 1;
  }

  const char *stun_port_s = stun_arg[0] ? split_port(stun_arg, "3478") : NULL;

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
      .signal_url = signal_url,
      .room = room,
      .stun_host = stun_arg[0] ? stun_arg : NULL,
      .stun_port = stun_port_s ? atoi(stun_port_s) : 0,
      .timeout_ms = 300000,    // five minutes for a human to type the code
      .ice_timeout_ms = 30000, // then give up cleanly; there is no fallback
      .on_room = show_room,
  };

  // Credentials go into the config and are never printed. Nothing in this
  // program logs cfg, and net.c only ever hands them to libjuice.
  for (int t = 0; t < turn_count; t++) {
    const char *tp = split_port(turn_arg[t], "3478");
    cfg.turn[t].host = turn_arg[t];
    cfg.turn[t].port = tp ? atoi(tp) : DEFAULT_TURN_PORT;
    cfg.turn[t].user = turn_user;
    cfg.turn[t].pass = turn_pass;
  }
  cfg.turn_count = turn_count;

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
  g->route = net_route(g->net);
  snprintf(g->status, sizeof g->status, "connected over %s", g->route);

  battle_loop(g);

  net_close(g->net);
  free(g);
  return 0;
}
