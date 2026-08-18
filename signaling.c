#define _POSIX_C_SOURCE 200809L

#include "signaling.h"

#include <libwebsockets.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUEUE 8

typedef struct {
  char *buf; // LWS_PRE bytes of headroom, then the payload
  size_t len;
} OutMsg;

struct Signal {
  struct lws_context *ctx;
  struct lws *wsi;
  int established;
  int dead;

  char rx[SIGNAL_MSG_MAX + 1]; // reassembly across websocket fragments
  size_t rxlen;

  char inq[QUEUE][SIGNAL_MSG_MAX + 1];
  int inhead, incount;

  OutMsg outq[QUEUE];
  int outhead, outcount;
};

static char last_err[256] = "";

const char *signal_error(void) { return last_err; }

static void set_err(const char *s) {
  snprintf(last_err, sizeof last_err, "%.*s", (int)sizeof last_err - 1, s);
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int signal_alive(const Signal *s) { return s && !s->dead; }

// ------------------------------------------------------------- callbacks ---

static void queue_in(Signal *s, const char *msg, size_t len) {
  if (s->incount >= QUEUE || len > SIGNAL_MSG_MAX)
    return; // a peer that floods us does not get to grow our memory
  char *slot = s->inq[(s->inhead + s->incount) % QUEUE];
  memcpy(slot, msg, len);
  slot[len] = '\0';
  s->incount++;
}

static void drain_out(Signal *s) {
  while (s->outcount > 0) {
    OutMsg *m = &s->outq[s->outhead];
    int n = lws_write(s->wsi, (unsigned char *)m->buf + LWS_PRE, m->len,
                      LWS_WRITE_TEXT);
    free(m->buf);
    m->buf = NULL;
    s->outhead = (s->outhead + 1) % QUEUE;
    s->outcount--;
    if (n < (int)m->len) {
      s->dead = 1;
      return;
    }
    if (s->outcount && lws_send_pipe_choked(s->wsi)) {
      lws_callback_on_writable(s->wsi);
      return;
    }
  }
}

static int callback_signaling(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
  (void)user;
  Signal *s = lws_context_user(lws_get_context(wsi));
  if (!s)
    return 0;

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    s->established = 1;
    if (s->outcount)
      lws_callback_on_writable(wsi);
    return 0;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    set_err(in ? (const char *)in : "signaling connection failed");
    s->dead = 1;
    s->wsi = NULL;
    return -1;

  case LWS_CALLBACK_CLIENT_WRITEABLE:
    drain_out(s);
    return 0;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    if (s->rxlen + len > SIGNAL_MSG_MAX) {
      set_err("signaling message too large");
      s->dead = 1;
      return -1;
    }
    memcpy(s->rx + s->rxlen, in, len);
    s->rxlen += len;

    if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi))
      return 0;

    queue_in(s, s->rx, s->rxlen);
    s->rxlen = 0;
    return 0;

  case LWS_CALLBACK_CLIENT_CLOSED:
    s->dead = 1;
    s->wsi = NULL;
    return 0;

  default:
    return 0;
  }
}

static const struct lws_protocols protocols[] = {
    {"warship-signaling", callback_signaling, 0, SIGNAL_MSG_MAX, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM,
};

// ------------------------------------------------------------------- api ---

int signal_connect(Signal **out, const char *url, int timeout_ms) {
  Signal *s = calloc(1, sizeof *s);
  if (!s) {
    set_err("out of memory");
    return 0;
  }

  // lws_parse_uri rewrites its argument in place, so it gets a copy.
  char urlbuf[512];
  if (snprintf(urlbuf, sizeof urlbuf, "%s", url) >= (int)sizeof urlbuf) {
    set_err("signaling url too long");
    goto fail;
  }

  const char *scheme, *host, *path;
  int port;
  if (lws_parse_uri(urlbuf, &scheme, &host, &port, &path)) {
    set_err("could not parse the signaling url");
    goto fail;
  }

  int tls;
  if (!strcmp(scheme, "wss") || !strcmp(scheme, "https"))
    tls = 1;
  else if (!strcmp(scheme, "ws") || !strcmp(scheme, "http"))
    tls = 0;
  else {
    set_err("signaling url must be ws:// or wss://");
    goto fail;
  }

  char fullpath[256];
  snprintf(fullpath, sizeof fullpath, "/%s", path); // lws strips the slash

  // Errors and warnings only: lws is chatty by default and this program owns
  // the terminal.
  lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

  struct lws_context_creation_info info = {0};
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.user = s;
  info.gid = (gid_t)-1;
  info.uid = (uid_t)-1;
  if (tls)
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

  s->ctx = lws_create_context(&info);
  if (!s->ctx) {
    set_err("could not create the websocket context");
    goto fail;
  }

  struct lws_client_connect_info ci = {0};
  ci.context = s->ctx;
  ci.address = host;
  ci.port = port;
  ci.path = fullpath;
  ci.host = host;
  ci.origin = host;
  ci.protocol = protocols[0].name;
  ci.pwsi = &s->wsi;
  if (tls)
    ci.ssl_connection = LCCSCF_USE_SSL;

  if (!lws_client_connect_via_info(&ci)) {
    set_err("could not start the signaling connection");
    goto fail;
  }

  long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 15000);
  while (!s->established && !s->dead && now_ms() < deadline)
    lws_service(s->ctx, 20);

  if (!s->established) {
    if (!last_err[0])
      set_err("timed out connecting to the signaling service");
    goto fail;
  }

  *out = s;
  return 1;

fail:
  if (s->ctx)
    lws_context_destroy(s->ctx);
  free(s);
  return 0;
}

void signal_close(Signal *s) {
  if (!s)
    return;
  while (s->outcount > 0) {
    free(s->outq[s->outhead].buf);
    s->outhead = (s->outhead + 1) % QUEUE;
    s->outcount--;
  }
  if (s->ctx)
    lws_context_destroy(s->ctx);
  free(s);
}

int signal_send(Signal *s, const char *msg) {
  if (!s || s->dead || !s->wsi)
    return 0;

  size_t len = strlen(msg);
  if (len > SIGNAL_MSG_MAX || s->outcount >= QUEUE) {
    set_err("signaling outbox full");
    return 0;
  }

  OutMsg *slot = &s->outq[(s->outhead + s->outcount) % QUEUE];
  slot->buf = malloc(LWS_PRE + len);
  if (!slot->buf) {
    set_err("out of memory");
    return 0;
  }
  memcpy(slot->buf + LWS_PRE, msg, len);
  slot->len = len;
  s->outcount++;

  // Requesting writability from outside the event loop is not enough on its
  // own: lws has already computed its poll set and does not see the request
  // until something wakes it. Without the cancel, the first message of a
  // connection goes out and later ones sit in the queue.
  lws_callback_on_writable(s->wsi);
  lws_cancel_service(s->ctx);
  return 1;
}

int signal_service(Signal *s, int timeout_ms) {
  if (!s || s->dead)
    return -1;
  lws_service(s->ctx, timeout_ms);
  return s->dead ? -1 : 0;
}

int signal_next(Signal *s, char *out, int cap) {
  if (!s || s->incount == 0)
    return 0;
  snprintf(out, (size_t)cap, "%s", s->inq[s->inhead]);
  s->inhead = (s->inhead + 1) % QUEUE;
  s->incount--;
  return 1;
}

// Pump until a message arrives or the deadline passes.
static int wait_message(Signal *s, char *out, int cap, int timeout_ms) {
  long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    if (signal_next(s, out, cap))
      return 1;
    if (s->dead) {
      if (!last_err[0])
        set_err("signaling connection closed");
      return -1;
    }
    if (now_ms() >= deadline)
      return 0;
    lws_service(s->ctx, 20);
  }
}

// An ERROR message is the service telling us why it is about to hang up.
static int is_error(const char *msg) {
  if (!strncmp(msg, "ERROR", 5)) {
    set_err(msg);
    return 1;
  }
  return 0;
}

int signal_create_room(Signal *s, char *code, int cap, int timeout_ms) {
  if (cap <= SIGNAL_CODE_LEN)
    return 0;
  if (!signal_send(s, "CREATE"))
    return 0;

  char msg[SIGNAL_MSG_MAX + 1];
  if (wait_message(s, msg, sizeof msg, timeout_ms) != 1) {
    if (!last_err[0])
      set_err("no reply from the signaling service");
    return 0;
  }
  if (is_error(msg))
    return 0;
  if (strncmp(msg, "ROOM ", 5) || (int)strlen(msg + 5) != SIGNAL_CODE_LEN) {
    set_err("unexpected reply to CREATE");
    return 0;
  }
  memcpy(code, msg + 5, SIGNAL_CODE_LEN + 1);
  return 1;
}

int signal_join_room(Signal *s, const char *code) {
  if ((int)strlen(code) != SIGNAL_CODE_LEN) {
    set_err("room codes are 6 characters");
    return 0;
  }
  char msg[32];
  snprintf(msg, sizeof msg, "JOIN %s", code);
  return signal_send(s, msg);
}

int signal_wait_peer(Signal *s, int *is_host, int timeout_ms) {
  char msg[SIGNAL_MSG_MAX + 1];
  int rc = wait_message(s, msg, sizeof msg, timeout_ms);
  if (rc == 0)
    set_err("nobody joined");
  if (rc != 1)
    return 0;
  if (is_error(msg))
    return 0;

  if (!strcmp(msg, "PEER host")) {
    *is_host = 1;
    return 1;
  }
  if (!strcmp(msg, "PEER guest")) {
    *is_host = 0;
    return 1;
  }
  set_err("expected PEER");
  return 0;
}
