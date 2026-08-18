// Named signaling.h, not signal.h: the latter would shadow the standard
// header for anything compiled with -I. in this directory.
#ifndef SIGNALING_H
#define SIGNALING_H

// Client for the rendezvous server (server/rendezvous.c). Plain TCP, used only
// until ICE connects -- once the peers have a direct path this socket closes and
// the game never touches it again.
//
// The fd is exposed so net.c can poll it alongside everything else: ICE trickles
// candidates, so DESC/CAND keep arriving while connectivity checks run.

// Big enough for a packed ICE description (newlines folded to '|'), which is
// the longest thing that crosses the signaling channel.
#define SIGNAL_LINE 1024
#define SIGNAL_CODE_LEN 6

typedef struct {
  int fd; // -1 when closed
  char in[SIGNAL_LINE + 1];
  int inlen;
} Signal;

int signal_connect(Signal *s, const char *host, const char *port);
int signal_fd(const Signal *s);
void signal_close(Signal *s);

// Write one line (a newline is appended). Returns 0 on failure.
int signal_send(Signal *s, const char *verb, const char *payload);

// Read one line, without its newline. 1 = got a line, 0 = timed out,
// -1 = closed or protocol violation. timeout_ms < 0 blocks.
int signal_line(Signal *s, char *out, int cap, int timeout_ms);

// Ask for a fresh room. Fills `code` (SIGNAL_CODE_LEN + 1 bytes). 0 on failure.
int signal_new_room(Signal *s, char *code, int cap, int timeout_ms);

// Enter an existing room. 0 on failure (bad code, room gone, room full).
int signal_join_room(Signal *s, const char *code, int timeout_ms);

// Block until the other player arrives. Sets *is_host. 0 on failure/timeout.
int signal_wait_peer(Signal *s, int *is_host, int timeout_ms);

// Last server-reported error, e.g. "ERR nosuchroom". Never a format string.
const char *signal_error(void);

#endif
