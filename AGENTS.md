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
make itest    # signaling, ICE and relay over loopback; starts its own server
make smoke    # two real processes on ptys through a full turn; needs python3
make rendezvous
```

`make test` must stay network-free and must stay fast. Anything needing a
socket belongs in `itest`.

## Layout, and the rule behind it

```
board.c    game rules. No I/O of any kind. State hangs off a Board*.
proto.c    wire format + stop-and-wait reliability. No sockets, no clock.
ui.c       the only file that touches the terminal.
signaling.c  TCP client for the rendezvous server.
net.c      libjuice + libplum, and the only file with a foreign thread.
main.c     argument parsing, poll() loop, game state machine.
server/rendezvous.c   the public daemon. Links nothing else in this repo.
```

The rule: **purity buys testability.** `board.c` and `proto.c` take their time
from the caller and own no globals, which is why packet loss, duplicate
delivery and retransmit timeouts are ordinary function calls in the tests. Do
not add I/O, a clock, or a global to either. If a new module can be pure, make
it pure.

`server/rendezvous.c` must never include a game header. If it needs one, the
layering is wrong.

## Invariants worth knowing before editing

- **Threads.** libjuice and libplum call back from their own threads. Every
  callback does exactly one thing: write a tagged record into the socketpair in
  `net.c`. The main loop owns all game state and the signaling socket. There
  are no mutexes and it should stay that way.
- **Trust boundaries.** `proto_parse` and `handle_line` in the server take
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
  `buildInputs` and the Makefile just says `-ljuice -lplum`.
- Writes to the signaling socket use `MSG_NOSIGNAL`. It carries game traffic
  once relaying, and a vanished peer would otherwise kill the process.
- Loopback ICE completes in roughly a millisecond. Do not try to force the
  relay path with a short timeout; use a negative `ice_timeout_ms`, which skips
  ICE outright.
- `snprintf` into a buffer that is also the source is an overlapping copy. It
  produced an empty room code once.

## Style

C11, `-Wall -Wextra`, warning-free. Comments explain *why*, especially why
something is deliberately simple or deliberately not. Match the density
already there rather than adding a header block to every function.

Non-trivial logic leaves one runnable check behind. No test frameworks.
