// Named signaling.h, not signal.h: the latter would shadow the standard
// header for anything compiled with -I. in this directory.
#ifndef SIGNALING_H
#define SIGNALING_H

// WebSocket client for the signaling service (server/signaling_server.c).
//
// Used only to find a peer and exchange ICE payloads. It carries NO game
// traffic and is closed as soon as ICE has a path, so nothing here appears in
// the game's event loop.
//
// The transport is ws:// or wss://, so it goes through a normal reverse proxy
// on 443 like any other web traffic.

#define SIGNAL_MSG_MAX 8192
#define SIGNAL_CODE_LEN 6

typedef struct Signal Signal;

// url is ws://host[:port][/path] or wss://... Returns 0 on failure.
int signal_connect(Signal **out, const char *url, int timeout_ms);
void signal_close(Signal *s);

int signal_alive(const Signal *s);
int signal_send(Signal *s, const char *msg); // one text frame

// Pump the websocket for up to timeout_ms. Returns 0 on success, -1 if the
// connection died. Must be called regularly while signaling is in use.
int signal_service(Signal *s, int timeout_ms);

// Take one received message, if there is one. 1 = got it, 0 = nothing queued.
int signal_next(Signal *s, char *out, int cap);

// Room helpers, all built on the four calls above.
int signal_create_room(Signal *s, char *code, int cap, int timeout_ms);
int signal_join_room(Signal *s, const char *code);
int signal_wait_peer(Signal *s, int *is_host, int timeout_ms);

const char *signal_error(void);

// Trace connects, sends and receives to stderr. Off by default; toggle before
// signal_connect().
void signal_set_verbose(int v);

#endif
