# Deploying warship

Two services: a signaling endpoint behind a reverse proxy, and coturn. Nothing
custom is exposed to the internet except the signaling websocket, and that
carries no game traffic.

`k8s/` is the pochi.casa deployment; `docker-compose.yml` and the Caddyfile
next to it are the generic single-host equivalent. Both describe the same
architecture.

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


## The pochi.casa deployment

```
                                signal.pochi.casa (AAAA -> sauron ::beef)
Player ── wss 443 ──► Caddy on sauron ── NodePort 30777 ──► warship-signal pod

                                turn.pochi.casa (AAAA -> tyr ::babe)
Player ── udp 3478 ─────────────────────────────────────►  coturn (hostNetwork)
Player ── udp 49160-49200 ──────────────────────────────►  relay allocations
```

Signaling follows the route `cuppy.pochi.casa` already uses: a NodePort on both
k3s nodes with Caddy on sauron in front. coturn cannot: TURN is UDP and not
HTTP, so there is nothing for a reverse proxy to do, and the relay hands out
the address it will relay from. It therefore runs with `hostNetwork` pinned to
tyr, and `turn.pochi.casa` points straight at tyr's public address.

### Apply

```sh
kubectl -n warship create secret generic coturn-credentials \
  --from-literal=password="$(openssl rand -base64 24)"

kubectl apply -k deploy/k8s
```

### DNS

Neither record exists yet. Both are AAAA only, matching the rest of the zone.

| Name | Type | Value | Why |
|---|---|---|---|
| `signal.pochi.casa` | AAAA | `2a07:7e81:85f5::beef` | sauron, which runs the Caddy that fronts the cluster |
| `turn.pochi.casa` | AAAA | `2a07:7e81:85f5::babe` | tyr, the only node coturn is pinned to |

### Changes needed in the infra repo

- `hosts/sauron/caddy.nix` — the `signal.pochi.casa` vhost.
- `hosts/tyr/default.nix` — 3478/udp, 3478/tcp and 49160-49200/udp.

### Checking it

```sh
export WARSHIP_TURN_PASSWORD=...
make prod-check
```

DNS, the websocket round trip, a real TURN allocation, and two clients pairing
through production. Run it after any change to either repo.
