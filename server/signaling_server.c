#define _POSIX_C_SOURCE 200809L

// Warship signaling service.
//
// Pairs two peers who know a room code and forwards opaque ICE payloads
// between them. That is the whole job. It does NOT relay game traffic: once
// ICE (or coturn) has a path, this service is finished and the socket closes.
//
// Protocol, one text frame per message:
//
//   -> CREATE               <- ROOM <code>
//   -> JOIN <code>          <- PEER host | PEER guest   (sent to both)
//   -> SIGNAL <payload>     <- SIGNAL <payload>         forwarded verbatim
//   -> BYE                  <- PEERGONE                 the other side left
//                           <- ERROR <reason>
//
// A plain HTTP GET answers 200 with a one-line status. The service speaks only
// websockets otherwise, so without this a browser or an uptime check gets a
// failed upgrade, which reads as an outage rather than as "wrong protocol".
//
// The payload of SIGNAL is never parsed here. It happens to be an SDP
// description or an ICE candidate, but this service has no opinion about that
// and must not grow one.
//
// TLS is deliberately absent: run it behind nginx or Caddy, which terminates
// wss:// on 443 and proxies plaintext to this port.

#include <libwebsockets.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define MAX_ROOMS 256
#define MAX_MSG 8192   // an SDP description plus a verb, with room to spare
#define MAX_QUEUE 8    // outbound messages held per client
#define CODE_LEN 6
// Waiting alone in a room. Generous on purpose: the code is only shown after
// the host has placed its fleet, and the other player has to receive it, start
// the program and place their own fleet before they can join. Two minutes
// covered someone already sitting at a terminal and nothing else.
#define LONELY_TIMEOUT_S 600
#define ROOM_LIFETIME_S 900 // an upper bound, not an expected duration

// No 0/o/1/l/i: codes get read aloud and typed by hand.
static const char CODE_ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";

typedef struct {
  char *buf; // LWS_PRE bytes of headroom, then the payload
  size_t len;
} OutMsg;

typedef struct Session {
  struct lws *wsi;
  int room; // index into rooms, or -1
  int slot; // 0 or 1 within the room
  long long joined_at;

  char in[MAX_MSG + 1]; // reassembly across websocket fragments
  size_t inlen;

  OutMsg out[MAX_QUEUE];
  int qhead, qcount;

  // Closing straight from the receive callback would discard whatever is
  // still queued, so a client would see a dropped connection instead of the
  // ERROR explaining why. Drain first, then close.
  int close_after_drain;
} Session;

typedef struct {
  int used;
  char code[CODE_LEN + 1];
  Session *peer[2];
  long long created_at;
} Room;

static Room rooms[MAX_ROOMS];
static struct lws_context *context;
static lws_sorted_usec_list_t sweep_sul;
static volatile sig_atomic_t interrupted;

static long long now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

// ------------------------------------------------------------- outbound ---

// Queue one message. Dropping on a full queue is deliberate: a client that
// will not drain is not one we grow memory for.
static void send_msg(Session *s, const char *msg) {
  if (!s || !s->wsi || s->qcount >= MAX_QUEUE)
    return;

  size_t len = strlen(msg);
  if (len > MAX_MSG)
    return;

  OutMsg *slot = &s->out[(s->qhead + s->qcount) % MAX_QUEUE];
  slot->buf = malloc(LWS_PRE + len);
  if (!slot->buf)
    return;
  memcpy(slot->buf + LWS_PRE, msg, len);
  slot->len = len;
  s->qcount++;
  lws_callback_on_writable(s->wsi);
}

static void send_error(Session *s, const char *reason) {
  char msg[128];
  // reason is ours, never client-supplied, but keep it bounded regardless.
  snprintf(msg, sizeof msg, "ERROR %.100s", reason);
  send_msg(s, msg);
}

// Report why, then hang up once the client has actually been told.
static void fail(Session *s, const char *reason) {
  send_error(s, reason);
  s->close_after_drain = 1;
}

static void drain_queue(Session *s) {
  while (s->qcount > 0) {
    OutMsg *m = &s->out[s->qhead];
    int n = lws_write(s->wsi, (unsigned char *)m->buf + LWS_PRE, m->len,
                      LWS_WRITE_TEXT);
    free(m->buf);
    m->buf = NULL;
    s->qhead = (s->qhead + 1) % MAX_QUEUE;
    s->qcount--;
    if (n < (int)m->len)
      return; // partial or failed write; lws will call us back
    if (s->qcount && lws_send_pipe_choked(s->wsi)) {
      lws_callback_on_writable(s->wsi);
      return;
    }
  }
}

static void free_queue(Session *s) {
  while (s->qcount > 0) {
    free(s->out[s->qhead].buf);
    s->out[s->qhead].buf = NULL;
    s->qhead = (s->qhead + 1) % MAX_QUEUE;
    s->qcount--;
  }
}

// ---------------------------------------------------------------- rooms ---

static int room_of_code(const char *code) {
  for (int i = 0; i < MAX_ROOMS; i++)
    if (rooms[i].used && !strcmp(rooms[i].code, code))
      return i;
  return -1;
}

// getrandom, not rand(): a guessable code lets a stranger walk into someone
// else's match before the real opponent does.
static int make_room(Session *s) {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].used)
      continue;

    Room *r = &rooms[i];
    r->code[0] = '\0';
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
    r->peer[0] = s;
    r->peer[1] = NULL;
    r->created_at = now_s();
    return i;
  }
  return -1;
}

static Session *other_peer(Session *s) {
  if (s->room < 0)
    return NULL;
  Room *r = &rooms[s->room];
  return (r->peer[0] == s) ? r->peer[1] : r->peer[0];
}

// Tear the room down. A half-populated room is useless to whoever is left, so
// tell them and let them reconnect rather than leaving them hanging.
static void leave_room(Session *s) {
  if (s->room < 0)
    return;
  Room *r = &rooms[s->room];
  Session *peer = other_peer(s);
  r->peer[0] = r->peer[1] = NULL;
  r->used = 0;
  s->room = -1;

  if (peer) {
    peer->room = -1;
    send_msg(peer, "PEERGONE");
    lws_callback_on_writable(peer->wsi);
  }
}

static int code_ok(const char *s) {
  if (strlen(s) != CODE_LEN)
    return 0;
  for (int i = 0; i < CODE_LEN; i++)
    if (!strchr(CODE_ALPHABET, s[i]))
      return 0;
  return 1;
}

// Payloads end up in the opponent's process. Allow printable ASCII and the
// newlines an SDP description legitimately contains; refuse everything else,
// so no one can smuggle terminal escapes to the other player.
static int payload_ok(const char *s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '\n' || c == '\r')
      continue;
    if (c < 0x20 || c >= 0x7f)
      return 0;
  }
  return 1;
}

// ------------------------------------------------------------- messages ---

// 0 means the connection should be closed.
static int handle_message(Session *s, char *msg) {
  if (!strcmp(msg, "CREATE")) {
    if (s->room >= 0) {
      fail(s, "already in a room");
      return 1;
    }
    int ri = make_room(s);
    if (ri < 0) {
      fail(s, "no rooms available");
      return 1;
    }
    s->room = ri;
    s->slot = 0;
    char reply[32];
    snprintf(reply, sizeof reply, "ROOM %s", rooms[ri].code);
    send_msg(s, reply);
    return 1;
  }

  if (!strncmp(msg, "JOIN ", 5)) {
    if (s->room >= 0) {
      fail(s, "already in a room");
      return 1;
    }
    const char *code = msg + 5;
    if (!code_ok(code)) {
      fail(s, "malformed room code");
      return 1;
    }
    int ri = room_of_code(code);
    if (ri < 0) {
      fail(s, "no such room");
      return 1;
    }
    if (rooms[ri].peer[1]) {
      fail(s, "room is full");
      return 1;
    }
    rooms[ri].peer[1] = s;
    s->room = ri;
    s->slot = 1;
    send_msg(rooms[ri].peer[0], "PEER host");
    send_msg(s, "PEER guest");
    return 1;
  }

  if (!strncmp(msg, "SIGNAL ", 7)) {
    // Validate before anything else: a malformed payload is a protocol
    // violation whether or not there is currently someone to forward it to.
    if (!payload_ok(msg + 7)) {
      fail(s, "payload rejected");
      return 1;
    }
    Session *peer = other_peer(s);
    if (!peer) {
      send_error(s, "no peer in room"); // recoverable: the peer may yet arrive
      return 1;
    }
    send_msg(peer, msg); // verbatim: the payload is not ours to interpret
    return 1;
  }

  if (!strcmp(msg, "BYE"))
    return 0;

  fail(s, "unknown message");
  return 1;
}

// --------------------------------------------------------------- plumbing ---

static void sweep_cb(lws_sorted_usec_list_t *sul) {
  (void)sul;
  long long t = now_s();
  for (int i = 0; i < MAX_ROOMS; i++) {
    Room *r = &rooms[i];
    if (!r->used)
      continue;

    int lonely = !r->peer[0] || !r->peer[1];
    int expired = t - r->created_at > ROOM_LIFETIME_S ||
                  (lonely && t - r->created_at > LONELY_TIMEOUT_S);
    if (!expired)
      continue;

    for (int k = 0; k < 2; k++) {
      if (!r->peer[k])
        continue;
      send_error(r->peer[k], "room expired");
      r->peer[k]->room = -1;
      lws_callback_on_writable(r->peer[k]->wsi);
    }
    r->peer[0] = r->peer[1] = NULL;
    r->used = 0;
  }
  lws_sul_schedule(context, 0, &sweep_sul, sweep_cb, 5 * LWS_US_PER_SEC);
}

static int callback_signaling(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
  Session *s = user;

  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED:
    memset(s, 0, sizeof *s);
    s->wsi = wsi;
    s->room = -1;
    s->joined_at = now_s();
    return 0;

  case LWS_CALLBACK_SERVER_WRITEABLE:
    drain_queue(s);
    if (s->close_after_drain && s->qcount == 0) {
      // Set a close reason so lws sends a proper close frame. A bare -1 is an
      // abrupt teardown and can discard the ERROR we just queued.
      lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
      return -1;
    }
    return 0;

  case LWS_CALLBACK_RECEIVE: {
    // Reassemble fragments, but never past the cap: an unbounded read loop is
    // a trivial memory denial of service.
    if (s->inlen + len > MAX_MSG) {
      fail(s, "message too large");
      s->inlen = 0;
      return 0;
    }
    memcpy(s->in + s->inlen, in, len);
    s->inlen += len;

    if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi))
      return 0;

    s->in[s->inlen] = '\0';
    size_t n = s->inlen;
    s->inlen = 0;
    if (memchr(s->in, '\0', n)) // embedded NUL: not a text message
      return -1;

    if (!handle_message(s, s->in))
      return -1;
    if (s->close_after_drain)
      lws_callback_on_writable(wsi);
    return 0;
  }

  case LWS_CALLBACK_CLOSED:
    leave_room(s);
    free_queue(s);
    s->wsi = NULL;
    return 0;

  default:
    return 0;
  }
}

// Anything that is not a websocket upgrade lands here, because lws routes
// plain HTTP to the vhost's first protocol.
static int callback_health(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user, void *in, size_t len) {
  (void)user;
  (void)in;
  (void)len;

  if (reason != LWS_CALLBACK_HTTP)
    return 0;

  static const char body[] = "warship signaling ok\n";
  unsigned char buf[LWS_PRE + 512];
  unsigned char *p = buf + LWS_PRE;
  unsigned char *end = buf + sizeof buf;

  if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "text/plain",
                                  sizeof body - 1, &p, end))
    return 1;
  if (lws_finalize_write_http_header(wsi, buf + LWS_PRE, &p, end))
    return 1;

  unsigned char out[LWS_PRE + sizeof body];
  memcpy(out + LWS_PRE, body, sizeof body - 1);
  if (lws_write(wsi, out + LWS_PRE, sizeof body - 1, LWS_WRITE_HTTP_FINAL) < 0)
    return 1;

  return lws_http_transaction_completed(wsi) ? -1 : 0;
}

static const struct lws_protocols protocols[] = {
    {"http", callback_health, 0, 0, 0, NULL, 0},
    {"warship-signaling", callback_signaling, sizeof(Session), MAX_MSG, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM,
};

static void on_signal(int sig) {
  (void)sig;
  interrupted = 1;
  lws_cancel_service(context);
}

int main(int argc, char **argv) {
  int port = 7777;
  if (argc > 1) {
    port = atoi(argv[1]);
    if (port < 1 || port > 65535) {
      fprintf(stderr, "usage: %s [port]\n", argv[0]);
      return 1;
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

  struct lws_context_creation_info info = {0};
  info.port = port;
  info.protocols = protocols;
  info.gid = (gid_t)-1;
  info.uid = (uid_t)-1;
  // No TLS here on purpose: nginx or Caddy terminates wss:// and proxies
  // plaintext to this port.
  info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

  context = lws_create_context(&info);
  if (!context) {
    fprintf(stderr, "lws_create_context failed\n");
    return 1;
  }
  fprintf(stderr, "warship signaling listening on %d (ws, no tls)\n", port);

  lws_sul_schedule(context, 0, &sweep_sul, sweep_cb, 5 * LWS_US_PER_SEC);
  while (!interrupted && lws_service(context, 0) >= 0)
    ;

  lws_context_destroy(context);
  return 0;
}
