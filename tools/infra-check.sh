#!/usr/bin/env bash
# Regression test against the real pochi.casa deployment.
#
#   make prod-check
#
# Everything here talks to production. It creates and abandons a couple of
# signaling rooms and, if credentials are present, one TURN allocation. It
# never writes to any host.
#
# Exit codes: 0 all good, 1 a check failed, 2 could not run a check at all.
set -uo pipefail

SIGNAL_HOST=${SIGNAL_HOST:-signal.pochi.casa}
TURN_HOST=${TURN_HOST:-turn.pochi.casa}
SIGNAL_URL=${SIGNAL_URL:-wss://${SIGNAL_HOST}/}
TURN_PORT=${TURN_PORT:-3478}

pass=0 fail=0 skip=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
skipd(){ printf '  --   %s (skipped: %s)\n' "$1" "$2"; skip=$((skip+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# --- 1. DNS ------------------------------------------------------------------
# *.pochi.casa is IPv6-only by design. An A record here would mean someone
# published a v4 address the rest of the stack does not expect.
head_ "DNS"
have_aaaa=1
for h in "$SIGNAL_HOST" "$TURN_HOST"; do
  aaaa=$(getent ahostsv6 "$h" 2>/dev/null | awk '{print $1}' | head -1)
  if [ -n "$aaaa" ]; then
    ok "$h has AAAA $aaaa"
  else
    bad "$h has no AAAA record"
    have_aaaa=0
  fi
done

# Only meaningful once the AAAA records exist: "no A record" is trivially true
# for a name that does not resolve at all.
if [ "$have_aaaa" -eq 0 ]; then
  skipd "IPv6-only check" "records missing"
elif ! command -v dig >/dev/null 2>&1; then
  skipd "IPv6-only check" "no dig"
else
  a=$(dig +short A "$TURN_HOST" | head -1)
  [ -z "$a" ] && ok "$TURN_HOST is IPv6-only" || bad "$TURN_HOST also has an A record ($a): unexpected for this deployment"
fi

# --- 2. Signaling ------------------------------------------------------------
# Proves TLS, the Caddy route, the NodePort, and that the service is answering
# the protocol rather than merely accepting connections.
head_ "Signaling"
if [ ! -x ./test_signaling ]; then
  skipd "websocket round trip" "run make test_signaling first"
else
  if out=$(./test_signaling "$SIGNAL_URL" 2>&1); then
    ok "room create, pair, SIGNAL forward, PEERGONE ($(echo "$out" | tail -1))"
  else
    bad "signaling round trip failed"
    echo "$out" | sed 's/^/       /' | tail -5
  fi
fi

# --- 3. TURN -----------------------------------------------------------------
# turnutils_uclient is the authoritative check: it allocates a real relay, so
# it catches a wrong password and a closed relay port range, which are the two
# failures that otherwise present as an unexplained ICE timeout.
head_ "TURN"
if [ -z "${WARSHIP_TURN_PASSWORD:-}" ]; then
  skipd "TURN allocation" "WARSHIP_TURN_PASSWORD not set"
elif ! command -v turnutils_uclient >/dev/null 2>&1; then
  skipd "TURN allocation" "no turnutils_uclient (nix shell nixpkgs#coturn)"
else
  if out=$(timeout 25 turnutils_uclient -T -6 -n 2 \
             -u "${WARSHIP_TURN_USER:-warship}" -w "$WARSHIP_TURN_PASSWORD" \
             -p "$TURN_PORT" "$TURN_HOST" 2>&1); then
    ok "relay allocation succeeded over IPv6"
  else
    bad "TURN allocation failed: check the password and the 49160-49200/udp range"
    echo "$out" | sed 's/^/       /' | tail -5
  fi
fi

# --- 4. End to end -----------------------------------------------------------
# Two real clients pairing through production signaling and connecting over
# ICE. This is the check that would have caught every regression so far.
head_ "End to end"
if [ ! -x ./test_net ]; then
  skipd "two-client game connect" "run make test_net first"
else
  if out=$(timeout 90 ./test_net "$SIGNAL_URL" 2>&1); then
    ok "$(echo "$out" | grep -E '^(route|net ok)' | tr '\n' ' ')"
  else
    bad "two clients could not establish a connection"
    echo "$out" | sed 's/^/       /' | tail -5
  fi
fi

# --- 5. Signaling is not a relay ---------------------------------------------
# Both peers above are on this machine, so they will have taken a direct path.
# What matters is that the websocket is gone afterwards: test_net asserts the
# room no longer exists, which is reported in its output above.
head_ "Summary"
printf '  %d passed, %d failed, %d skipped\n\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ] || exit 1
[ "$pass" -gt 0 ] || exit 2
