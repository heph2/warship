# AGENTS.md

Terminal Battleship over the network, in C11. Built as a learning project: the
owner writes most of it, so prefer explaining a mechanism over silently
producing code, and keep changes small and reviewable.

## Build and test

Everything runs inside the nix dev shell (`direnv reload` after touching
`flake.nix`).

```sh
make          # warship
make test     # board rules, protocol, candidate parsing -- no network
make itest    # signaling and ICE over loopback; starts its own server
make smoke    # two real processes on ptys through a full turn; needs python3
make signal-server
```

`make test` must stay network-free and must stay fast. Anything needing a
socket belongs in `itest`.

## Layout, and the rule behind it

```
board.c    game rules. No I/O of any kind. State hangs off a Board*.
proto.c    wire format + stop-and-wait reliability. No sockets, no clock.
ui.c       the only file that touches the terminal.
signaling.c  websocket client for the signaling service.
net.c      libjuice + libplum, and the only file with a foreign thread.
main.c     argument parsing, poll() loop, game state machine.
server/signaling_server.c   the public daemon. Links nothing else in this repo.
```

The rule: **purity buys testability.** `board.c` and `proto.c` take their time
from the caller and own no globals, which is why packet loss, duplicate
delivery and retransmit timeouts are ordinary function calls in the tests. Do
not add I/O, a clock, or a global to either. If a new module can be pure, make
it pure.

`server/signaling_server.c` must never include a game header. If it needs one,
the layering is wrong.

**Signaling must never carry game traffic.** It exists to pair two players and
pass opaque ICE payloads, and `net_open` closes it the instant ICE connects.
When no path can be found the answer is a clean error, not a fallback: coturn
is the only relay. `test_net` proves the websocket is gone after connecting by
checking the room no longer exists.

## Invariants worth knowing before editing

- **Threads.** libjuice and libplum call back from their own threads. Every
  callback does exactly one thing: write a tagged record into the socketpair in
  `net.c`. The main loop owns all game state and the signaling socket. There
  are no mutexes and it should stay that way.
- **Trust boundaries.** `proto_parse` and `handle_message` in the server take
  bytes from strangers. Range-check numbers, reject trailing garbage, reject
  unprintable bytes. An ANSI escape in a nick rewrites the opponent's terminal.
- **Frames never contain newlines.** `ui.c` positions everything with explicit
  `ESC[row;colH` and writes one buffer per frame. A stray `\n` reintroduces
  scrolling and flicker.
- **The transport is unreliable.** ICE is not DTLS and not SCTP. Loss,
  duplication and reordering are `proto.c`'s job. Do not paper over them
  elsewhere.
- **Duplicate square vs duplicate datagram** are different bugs with different
  answers. `FIRE_REJECT` is a player firing twice; sequence dedupe is the
  network delivering once twice.
- Two messages may be in flight, so the outbox is a FIFO. A single slot
  deadlocks when an ACK is lost.

## Gotchas that have already cost time

- The header is `signaling.h`, not `signal.h`, which would shadow the standard
  header.
- libjuice and libplum are **not in nixpkgs**. `flake.nix` builds both from
  source, pinned by tag and hash. libjuice ships no `.pc` file, so they live in
  `buildInputs` and the Makefile just says `-ljuice -lplum -lwebsockets`.
  libwebsockets comes from nixpkgs but its public header includes
  `<openssl/ssl.h>`, so OpenSSL has to be in `buildInputs` too.
- **libjuice does TURN over UDP only.** `juice_turn_server_t` has no transport
  field, so `turns:` on 5349 cannot be configured. Do not add a flag for it.
- `lws_callback_on_writable()` called from outside the event loop is not enough
  on its own; pair it with `lws_cancel_service()` or only the first message of
  a connection is ever sent. This cost an afternoon.
- One libwebsockets context per process. Interleaving several in one thread is
  a shape the product never has, and the tests fork rather than try.
- Loopback ICE completes in roughly a millisecond, so timing knobs are a poor
  way to force a particular path in tests.
- `typ prflx` is a normal outcome, not an anomaly. A peer-reflexive candidate
  is an address learned from an incoming connectivity check rather than from
  signaling, and it appears whenever a candidate arrives later than packets
  sent from it -- roughly 40% of connections through the real deployment, and
  almost never on loopback. `net_route` must handle it or a perfectly good
  direct path reports itself as "unknown".
- `net_route` is briefly "unknown" right after connecting: the selected pair
  is not immediately readable. Re-read it rather than latching the first
  answer, and sleep between attempts -- spinning on it starves the libjuice
  thread that has to publish the pair.
- The coturn image's turnserver carries a file capability, so `capabilities:
  drop: ["ALL"]` alone makes execve fail with EPERM and the pod crash-loops
  with "Operation not permitted". It needs NET_BIND_SERVICE back in the
  bounding set even though it binds nothing privileged.
- Signaling ending mid-handshake is **not** a failure. Whoever completes ICE
  first closes its websocket, which collapses the room and hands the other peer
  a `PEERGONE` while it is still finishing. It only means no more candidates
  are coming.
- `snprintf` into a buffer that is also the source is an overlapping copy. It
  produced an empty room code once.

## Style

C11, `-Wall -Wextra`, warning-free. Comments explain *why*, especially why
something is deliberately simple or deliberately not. Match the density
already there rather than adding a header block to every function.

Non-trivial logic leaves one runnable check behind. No test frameworks.
