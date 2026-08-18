// Signaling server for warship. Runs on a public host.
//
// It pairs two peers who know a room code and forwards opaque lines between
// them so they can exchange ICE descriptions. It never parses ICE, never sees
// the game, and links nothing else in this repo -- if it ever needs board.h,
// the layering is wrong.
//
//   -> NEW                first peer: server invents a code
//   <- ROOM <code>
//   <- WAITING
//   -> JOIN <code>        second peer
//   <- PEER host | PEER guest    (sent to both once the room is full)
//   -> DESC <blob>        forwarded verbatim to the other peer
//   -> CAND <candidate>   forwarded verbatim, repeatable (trickle ICE)
//   -> DONE               forwarded: local candidate gathering finished
//   -> RELAY <payload>    forwarded: a game datagram, when ICE could not find
//                         a direct path. This is the relay of last resort, so
//                         the room has to outlive signaling.
//   -> BYE
//
// Everything arriving here is from an unauthenticated stranger on the open
// internet. Every limit below exists because its absence is a denial of
// service, not because it is tidy.

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 512
#define MAX_ROOMS 256
#define LINE_MAX 1024 // must hold a packed ICE description
#define CODE_LEN 6
#define LONELY_TIMEOUT_S 60 // no partner showed up
// Rooms used to have a flat lifetime, which was fine when they only carried
// signaling. Now a room may be relaying a whole game, so it expires on being
// idle instead -- long enough to think about a move, short enough to reclaim.
#define ROOM_IDLE_S 900

// No 0/o/1/l/i: codes get read aloud and typed by hand.
static const char CODE_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

typedef struct {
  int fd; // -1 == slot free
  int room;
  char in[LINE_MAX + 1];
  int inlen;
  long long joined_at;
} Client;

typedef struct {
  int used;
  char code[CODE_LEN + 1];
  int peer[2]; // client indices, -1 when empty
  long long last_activity;
} Room;

static Client clients[MAX_CLIENTS];
static Room rooms[MAX_ROOMS];

static long long now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static void send_line(int ci, const char *s) {
  if (ci < 0 || clients[ci].fd < 0)
    return;
  // Never a format string: `s` can contain peer-supplied bytes.
  size_t n = strlen(s);
  ssize_t w = write(clients[ci].fd, s, n);
  (void)w; // a failed write is discovered by the next read returning 0
}

static void drop_client(int ci, const char *why) {
  Client *c = &clients[ci];
  if (c->fd < 0)
    return;
  if (why)
    send_line(ci, why);

  if (c->room >= 0) {
    Room *r = &rooms[c->room];
    for (int k = 0; k < 2; k++) {
      if (r->peer[k] == ci)
        r->peer[k] = -1;
      else if (r->peer[k] >= 0)
        drop_client(r->peer[k], "PEERGONE\n"); // a half-room is useless
    }
    r->used = 0;
  }
  close(c->fd);
  c->fd = -1;
  c->room = -1;
  c->inlen = 0;
}

static int room_of_code(const char *code) {
  for (int i = 0; i < MAX_ROOMS; i++)
    if (rooms[i].used && !strcmp(rooms[i].code, code))
      return i;
  return -1;
}

// getrandom, not rand(): a guessable code lets a stranger walk into someone
// else's match before the real opponent does.
static int make_room(int ci) {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].used)
      continue;

    Room *r = &rooms[i];
    for (int attempt = 0; attempt < 8; attempt++) {
      unsigned char raw[CODE_LEN];
      if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw)
        return -1;
      for (int k = 0; k < CODE_LEN; k++)
        r->code[k] = CODE_ALPHABET[raw[k] % (sizeof CODE_ALPHABET - 1)];
      r->code[CODE_LEN] = '\0';
      if (room_of_code(r->code) < 0)
        break;
      r->code[0] = '\0';
    }
    if (!r->code[0])
      return -1;

    r->used = 1;
    r->peer[0] = ci;
    r->peer[1] = -1;
    r->last_activity = now_s();
    return i;
  }
  return -1;
}

static int other_peer(int ci) {
  Client *c = &clients[ci];
  if (c->room < 0)
    return -1;
  Room *r = &rooms[c->room];
  return (r->peer[0] == ci) ? r->peer[1] : r->peer[0];
}

static int code_ok(const char *s) {
  if (strlen(s) != CODE_LEN)
    return 0;
  for (int i = 0; i < CODE_LEN; i++)
    if (!strchr(CODE_ALPHABET, s[i]))
      return 0;
  return 1;
}

// Forwarded payloads land in the other player's process. Refuse control bytes
// so a peer cannot smuggle ANSI escapes into an opponent's terminal.
static int printable(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f)
      return 0;
  return 1;
}

static void handle_line(int ci, char *line) {
  Client *c = &clients[ci];

  if (!printable(line)) {
    drop_client(ci, "ERR badbytes\n");
    return;
  }

  if (!strcmp(line, "NEW")) {
    if (c->room >= 0) {
      drop_client(ci, "ERR already\n");
      return;
    }
    int ri = make_room(ci);
    if (ri < 0) {
      drop_client(ci, "ERR full\n");
      return;
    }
    c->room = ri;
    char msg[32];
    snprintf(msg, sizeof msg, "ROOM %s\n", rooms[ri].code);
    send_line(ci, msg);
    send_line(ci, "WAITING\n");
    return;
  }

  if (!strncmp(line, "JOIN ", 5)) {
    if (c->room >= 0) {
      drop_client(ci, "ERR already\n");
      return;
    }
    const char *code = line + 5;
    if (!code_ok(code)) {
      drop_client(ci, "ERR badcode\n");
      return;
    }
    int ri = room_of_code(code);
    if (ri < 0) {
      drop_client(ci, "ERR nosuchroom\n");
      return;
    }
    if (rooms[ri].peer[1] >= 0) {
      drop_client(ci, "ERR full\n");
      return;
    }
    rooms[ri].peer[1] = ci;
    rooms[ri].last_activity = now_s();
    c->room = ri;
    send_line(rooms[ri].peer[0], "PEER host\n");
    send_line(ci, "PEER guest\n");
    return;
  }

  if (!strncmp(line, "DESC ", 5) || !strncmp(line, "CAND ", 5) ||
      !strncmp(line, "RELAY ", 6) || !strcmp(line, "DONE")) {
    int peer = other_peer(ci);
    if (peer < 0) {
      send_line(ci, "ERR nopeer\n");
      return;
    }
    rooms[c->room].last_activity = now_s();
    char msg[LINE_MAX + 2];
    snprintf(msg, sizeof msg, "%s\n", line); // opaque: never inspected
    send_line(peer, msg);
    return;
  }

  if (!strcmp(line, "BYE")) {
    drop_client(ci, NULL);
    return;
  }

  drop_client(ci, "ERR unknown\n");
}

static void read_client(int ci) {
  Client *c = &clients[ci];
  int space = LINE_MAX - c->inlen;
  if (space <= 0) { // a line longer than the cap: unbounded buffering, refuse
    drop_client(ci, "ERR toolong\n");
    return;
  }

  ssize_t n = read(c->fd, c->in + c->inlen, (size_t)space);
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      return;
    drop_client(ci, NULL);
    return;
  }
  c->inlen += (int)n;

  int start = 0;
  for (int i = 0; i < c->inlen; i++) {
    if (c->in[i] != '\n')
      continue;
    c->in[i] = '\0';
    int end = i;
    if (end > start && c->in[end - 1] == '\r')
      c->in[end - 1] = '\0';
    handle_line(ci, c->in + start);
    if (c->fd < 0)
      return; // handle_line dropped us
    start = i + 1;
  }
  memmove(c->in, c->in + start, (size_t)(c->inlen - start));
  c->inlen -= start;
}

static void sweep_timeouts(void) {
  long long t = now_s();
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].fd < 0)
      continue;
    int lonely = clients[i].room < 0 || other_peer(i) < 0;
    if (lonely && t - clients[i].joined_at > LONELY_TIMEOUT_S)
      drop_client(i, "ERR timeout\n");
  }
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].used)
      continue;
    if (t - rooms[i].last_activity > ROOM_IDLE_S) {
      for (int k = 0; k < 2; k++)
        if (rooms[i].peer[k] >= 0)
          drop_client(rooms[i].peer[k], "ERR expired\n");
      rooms[i].used = 0;
    }
  }
}

static int listen_on(int port) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  int on = 1, off = 0;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); // v4 too

  struct sockaddr_in6 a = {.sin6_family = AF_INET6,
                           .sin6_addr = in6addr_any,
                           .sin6_port = htons((unsigned short)port)};
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    perror("bind");
    return -1;
  }
  if (listen(fd, 32) < 0) {
    perror("listen");
    return -1;
  }
  return fd;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return 1;
  }
  int port = atoi(argv[1]);
  if (port < 1 || port > 65535) {
    fprintf(stderr, "bad port\n");
    return 1;
  }

  // A peer vanishing mid-write must not kill the daemon.
  signal(SIGPIPE, SIG_IGN);

  for (int i = 0; i < MAX_CLIENTS; i++) {
    clients[i].fd = -1;
    clients[i].room = -1;
  }

  int lfd = listen_on(port);
  if (lfd < 0)
    return 1;
  fprintf(stderr, "rendezvous listening on %d\n", port);

  for (;;) {
    struct pollfd fds[MAX_CLIENTS + 1];
    int map[MAX_CLIENTS + 1];
    int nfds = 0;

    fds[nfds] = (struct pollfd){.fd = lfd, .events = POLLIN};
    map[nfds++] = -1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].fd < 0)
        continue;
      fds[nfds] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN};
      map[nfds++] = i;
    }

    if (poll(fds, (nfds_t)nfds, 1000) < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      return 1;
    }

    for (int k = 1; k < nfds; k++)
      if (fds[k].revents & (POLLIN | POLLHUP | POLLERR))
        read_client(map[k]);

    if (fds[0].revents & POLLIN) {
      int fd = accept(lfd, NULL, NULL);
      if (fd >= 0) {
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
          if (clients[i].fd < 0) {
            slot = i;
            break;
          }
        if (slot < 0) {
          (void)!write(fd, "ERR busy\n", 9); // shed load, do not grow
          close(fd);
        } else {
          clients[slot] = (Client){.fd = fd, .room = -1, .joined_at = now_s()};
        }
      }
    }

    sweep_timeouts();
  }
}
