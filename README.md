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
nix develop          # or install libjuice + libplum yourself
make

./warship host --signal your.server:7777    # prints a room code
./warship join <code> --signal your.server:7777
```

`wasd` to move, `r` to rotate, `space` to place and to fire, `q` to quit.

## Connecting

ICE via [libjuice](https://github.com/paullouisageneau/libjuice), with two additions so a relay is rarely needed:

- **Port mapping** — [libplum](https://github.com/paullouisageneau/libplum) asks the router directly over NAT-PMP, PCP or UPnP-IGD. STUN only discovers what a NAT already did; this asks it to cooperate.
- **Relay of last resort** — when no direct path exists, the game falls back to the rendezvous connection it already has. No TURN deployment required, though TURN is used when configured.

The status line reports which path won: `host`, `port-mapped`, `srflx`, `relay` or `signal-relay`.

## Server

One small binary, the only thing that needs a public address.

```sh
docker build -t warship-rendezvous ./server
docker run -d --restart=unless-stopped -p 7777:7777 warship-rendezvous
```

Under 200 kB. It pairs peers by room code, forwards opaque lines, and never parses ICE.

## Tests

```sh
make test     # game rules and protocol, no network
make itest    # signaling, ICE and relay over loopback
make smoke    # two real processes on ptys, playing a turn
```
