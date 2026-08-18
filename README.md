```
                            |
                            |
                        ____|____
                       | o  o  o |
     __===__   ________|_________|________   __===__
    |_______| |                           | |_______|
    |_________|___________________________|_________|
     \                                             /
      \___________________________________________/
   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
```

# warship

Terminal Battleship played over the internet. Written in C, for learning.

Two players, one room code. No accounts, no lobby, no launcher.

## Play

```sh
nix develop          # or install libjuice, libplum and libwebsockets yourself
make

./warship host           # prints a room code
./warship join <code>
```

`wasd` to move, `r` to rotate, `space` to place and to fire, `q` to quit.

## Connecting

ICE via [libjuice](https://github.com/paullouisageneau/libjuice). Game traffic
only ever travels over ICE — direct, through a port the router opened, or via a
coturn relay candidate.

- **Signaling** is a small WebSocket service used to pair two players by room
  code and pass ICE payloads between them. It closes the moment ICE connects and
  never carries a game packet.
- **Port mapping** — [libplum](https://github.com/paullouisageneau/libplum) asks
  the router directly over NAT-PMP, PCP or UPnP-IGD. STUN only discovers what a
  NAT already did; this asks it to cooperate, which keeps most games off the
  relay.
- **coturn** is the only relay. If no path exists at all, you get a clean error
  rather than a silent fallback.

The status line reports which path won: `host`, `port-mapped`, `srflx` or `relay`.

## Server

Signaling behind a reverse proxy, plus coturn. See [deploy/](deploy/) for a
compose file, a Caddyfile, an nginx alternative and a coturn config.

```sh
kubectl apply -k deploy/k8s      # or: cd deploy && docker compose up -d
```

The signaling service pairs peers by room code and forwards opaque payloads. It
does not parse ICE, terminate TLS, or relay anything.

## Tests

```sh
make test     # game rules and protocol, no network
make itest    # signaling and ICE over loopback, plus the no-path failure
make smoke    # two real processes on ptys, playing a turn
make prod-check  # DNS, signaling, TURN and a real connection, against production
```
