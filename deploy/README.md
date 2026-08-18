# Deploying warship

Two services on one host: a signaling endpoint behind a reverse proxy, and
coturn. Nothing custom is exposed to the internet except the signaling
websocket, and that carries no game traffic.

```
                 443/tcp  HTTPS + WSS
Internet  ──────────────────────────►  Caddy  ──►  signal-server  (plaintext)

          3478/udp, 3478/tcp
Internet  ──────────────────────────►  coturn
          49160-49200/udp relay
```

## Setup

1. Point `signal.example.com` and `turn.example.com` at the host.
2. Edit `Caddyfile` (hostname), `turnserver.conf` (`realm`, and **change the
   `user=` password**).
3. `docker compose up -d`

## Ports

| Port | Purpose |
|---|---|
| 443/tcp | HTTPS and WSS signaling |
| 80/tcp | ACME challenge for the certificate |
| 3478/udp | STUN and TURN |
| 3478/tcp | TURN for networks that block UDP |
| 49160-49200/udp | **coturn relay range** |

The relay range is the one people forget. Without it TURN completes its
handshake and then relays nothing, which looks like a mysterious ICE failure
rather than a firewall problem.

There is no 5349/tls port. libjuice's `juice_turn_server_t` has no transport
field, so it speaks TURN over UDP only and could not use it.

## Playing

```sh
export WARSHIP_TURN_USER=warship
export WARSHIP_TURN_PASSWORD=...        # keeps it out of ps and shell history

./warship host \
  --signal-url wss://signal.example.com/ \
  --stun-server turn.example.com:3478 \
  --turn-server turn.example.com:3478

./warship join <code> \
  --signal-url wss://signal.example.com/ \
  --stun-server turn.example.com:3478 \
  --turn-server turn.example.com:3478
```

## Checking it works

The status line names the path that won: `host`, `port-mapped`, `srflx` or
`relay`. Only `relay` means coturn is carrying the game.

To prove the relay works rather than hoping, block direct connectivity between
the two machines and confirm the route becomes `relay`. If it instead fails,
the relay port range is the first thing to check.

```sh
docker compose logs coturn | grep -i allocat
```

## Credentials

Long-term credentials are configured here: one username and password shared by
all players. Simple, and fine when the players are people you know.

REST-API ephemeral credentials (`use-auth-secret`) are the better answer for a
public deployment: the signaling service derives a short-lived username and
password from a shared secret, so a leaked credential expires instead of
becoming a permanent free relay. The cost is that the signaling service must
then hold that secret and hand out credentials, which is a responsibility it
deliberately does not have today. Worth doing before opening this to strangers,
not before.
