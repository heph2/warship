# The signaling service. It is the only part that runs unattended on a host
# with a public address; the game is a terminal program.
#
#   docker build -t warship-signal ./server
#   docker run -d --restart=unless-stopped -p 7777:7777 warship-signal
#
# No TLS in here on purpose. Terminate wss:// at nginx or Caddy and proxy
# plaintext to this port, so certificates are handled by software that is
# already good at it.
#
# This used to be a static scratch image. libwebsockets ended that: the runtime
# now needs the shared library and its OpenSSL headers, which is the price of
# not hand-rolling a websocket implementation.

FROM alpine:3.20 AS build
RUN apk add --no-cache gcc musl-dev libwebsockets-dev openssl-dev
WORKDIR /src
COPY signaling_server.c .
RUN gcc -Wall -Wextra -Werror -std=c11 -O2 -o signal-server signaling_server.c \
      -lwebsockets

FROM alpine:3.20
RUN apk add --no-cache libwebsockets && adduser -D -u 65532 warship
COPY --from=build /src/signal-server /usr/local/bin/signal-server
USER warship
EXPOSE 7777/tcp
ENTRYPOINT ["/usr/local/bin/signal-server"]
CMD ["7777"]
